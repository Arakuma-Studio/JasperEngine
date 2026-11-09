// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// 2D sorting's order (docs/specs/S52-sprite-instancing.md R1-R9, D1, D2, AC1), on the CPU: the
// order RenderSystem gathers for the passes without distance sort. With every renderer at its
// defaults, no enabled Sorting Group and mode Default nothing is gathered (the hierarchy loop runs
// as it was). Otherwise the hierarchy is cut into runs at UI subtrees, which keep their place;
// inside a run, renderers draw by (Sorting Layer value, Order in Layer, axis distance in Custom
// Axis, hierarchy index); a Sorting Group sorts as one item, its members among themselves; a
// disabled group is transparent. Each draw's batcher bucket is its layer and order, a grouped
// one's the outermost group's with its sequence. The draws' images: SpriteSortGpuTest.cpp.

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/RectTransformComponent.h"
#include "component/SortingGroupComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"
#include "registry/SortingLayers.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    class SpriteSortTest : public ::testing::Test {
    protected:
        Scene scene;
        std::map<std::string, Entity> named;

        void SetUp() override{
            // layer values: Back 0, Default 1, Front 2 (ids 3, 0, 4)
            SortingLayers::setLayers({{3, "Back"}, {0, "Default"}, {4, "Front"}});
        }

        void TearDown() override{
            SortingLayers::reset();
        }

        Entity sprite(const std::string& name, int layer = 0, int order = 0, Entity parent = NULL_ENTITY, Vector3 position = Vector3(0, 0, 0)){
            const Entity entity = scene.createUserEntity();
            scene.addComponent<Transform>(entity, {});
            MeshComponent mesh;
            mesh.sortingLayer = layer;
            mesh.sortingOrder = order;
            scene.addComponent<MeshComponent>(entity, mesh);
            scene.addComponent<SpriteComponent>(entity, {});
            if (parent != NULL_ENTITY)
                scene.addEntityChild(parent, entity, false);
            Transform& transform = scene.getComponent<Transform>(entity);
            transform.position = position;
            transform.worldPosition = position;
            named[name] = entity;
            return entity;
        }

        Entity empty(const std::string& name, Entity parent = NULL_ENTITY){
            const Entity entity = scene.createUserEntity();
            scene.addComponent<Transform>(entity, {});
            if (parent != NULL_ENTITY)
                scene.addEntityChild(parent, entity, false);
            named[name] = entity;
            return entity;
        }

        Entity ui(const std::string& name, Entity parent = NULL_ENTITY){
            const Entity entity = empty(name, parent);
            scene.addComponent<RectTransformComponent>(entity, {});
            return entity;
        }

        Entity group(const std::string& name, int layer, int order, Entity parent = NULL_ENTITY, bool enabled = true){
            const Entity entity = empty(name, parent);
            SortingGroupComponent component;
            component.sortingLayer = layer;
            component.sortingOrder = order;
            component.enabled = enabled;
            scene.addComponent<SortingGroupComponent>(entity, component);
            return entity;
        }

        RenderSystem& render(){
            return *scene.getSystem<RenderSystem>();
        }

        // the names in the gathered order (entities without a name left out)
        std::vector<std::string> order(){
            render().updateSortingActive();
            std::vector<std::string> names;
            auto transforms = scene.getComponentArray<Transform>();
            for (const RenderSystem::SortedDraw& draw : render().getSorted2DOrder()){
                const Entity entity = transforms->getEntity(draw.index);
                for (const auto& [name, e] : named){
                    if (e == entity)
                        names.push_back(name);
                }
            }
            return names;
        }

        uint64_t bucketOf(const std::string& name){
            auto transforms = scene.getComponentArray<Transform>();
            for (const RenderSystem::SortedDraw& draw : render().getSorted2DOrder()){
                if (transforms->getEntity(draw.index) == named[name])
                    return draw.bucket;
            }
            return 0;
        }
    };

}

// R9: defaults gather nothing (one flag), and the 2D loop keeps the hierarchy order
TEST_F(SpriteSortTest, DefaultsGatherNothing){
    sprite("a");
    sprite("b");
    sprite("c");
    const uint32_t gathers = render().getSortGatherCount();
    render().updateSortingActive();
    EXPECT_FALSE(render().isSortingActive());
    EXPECT_TRUE(render().getSorted2DOrder().empty());
    EXPECT_EQ(render().getSortGatherCount(), gathers);
    // a disabled Sorting Group alone does not turn it on either
    group("g", 4, 3, NULL_ENTITY, false);
    render().updateSortingActive();
    EXPECT_FALSE(render().isSortingActive());
}

// R3: layer value, then Order in Layer, then the hierarchy
TEST_F(SpriteSortTest, LayersThenOrderThenHierarchy){
    sprite("front", 4);
    sprite("default+5", 0, 5);
    sprite("back", 3);
    sprite("default-1", 0, -1);
    sprite("default", 0, 0);
    EXPECT_EQ(order(), (std::vector<std::string>{"back", "default-1", "default", "default+5", "front"}));
    EXPECT_TRUE(render().isSortingActive());
    // an unknown layer id draws on Default (between default-1 and default+5, in hierarchy order)
    scene.getComponent<MeshComponent>(named["front"]).sortingLayer = 77;
    EXPECT_EQ(order(), (std::vector<std::string>{"back", "default-1", "front", "default", "default+5"}));
}

// R8: the order is total; equal keys keep the hierarchy order however many there are
TEST_F(SpriteSortTest, TiesKeepTheHierarchyOrder){
    std::vector<std::string> expectedBack, expectedDefault, expectedFront;
    for (int i = 0; i < 120; i++){
        const std::string name = "s" + std::to_string(i);
        const int pick = (i * 7) % 3;
        sprite(name, pick == 0 ? 3 : (pick == 1 ? 0 : 4));
        (pick == 0 ? expectedBack : (pick == 1 ? expectedDefault : expectedFront)).push_back(name);
    }
    std::vector<std::string> expected = expectedBack;
    expected.insert(expected.end(), expectedDefault.begin(), expectedDefault.end());
    expected.insert(expected.end(), expectedFront.begin(), expectedFront.end());
    EXPECT_EQ(order(), expected);
    // the same again (deterministic)
    EXPECT_EQ(order(), expected);
}

// R3: a UI subtree keeps its place and order (a run boundary); renderers sort within each run
TEST_F(SpriteSortTest, UISubtreesKeepTheirPlace){
    sprite("front1", 4);
    sprite("back1", 3);
    const Entity canvas = ui("canvas");
    ui("button", canvas);
    sprite("ui-sprite", 3, 0, canvas);
    sprite("front2", 4);
    sprite("back2", 3);
    EXPECT_EQ(order(), (std::vector<std::string>{"back1", "front1", "canvas", "button", "ui-sprite", "back2", "front2"}));
    EXPECT_EQ(bucketOf("button"), RenderSystem::UI_SORT_BUCKET);
}

// R5: a Sorting Group sorts as one item at its layer and order; its members among themselves;
// groups nest; a disabled one is transparent
TEST_F(SpriteSortTest, SortingGroupsSortAsOneItem){
    sprite("default+5", 0, 5);
    const Entity g = group("group", 0, 0);
    sprite("member+10", 4, 10, g);
    sprite("member-10", 3, -10, g);
    sprite("back", 3);
    // (the group's own GameObject, no renderer, is a member at the defaults)
    EXPECT_EQ(order(), (std::vector<std::string>{"back", "member-10", "group", "member+10", "default+5"}));
    // the members' bucket is the group's (its layer and order, its sequence)
    EXPECT_EQ(bucketOf("member+10"), bucketOf("member-10"));
    EXPECT_NE(bucketOf("member+10"), bucketOf("back"));

    // nested: the inner group is one item among the outer group's members
    const Entity inner = group("inner", 4, -5, g);
    sprite("inner-a", 0, 3, inner);
    sprite("inner-b", 0, 1, inner);
    EXPECT_EQ(order(), (std::vector<std::string>{"back", "member-10", "group", "inner", "inner-b", "inner-a", "member+10", "default+5"}));
    EXPECT_EQ(bucketOf("inner-a"), bucketOf("member-10")) << "a nested group takes its outermost group's bucket";

    // disabled: its children sort with the run they are in
    scene.getComponent<SortingGroupComponent>(g).enabled = false;
    const std::vector<std::string> flat = order();
    EXPECT_EQ(flat.front(), "member-10");
    EXPECT_EQ(flat.back(), "member+10");
}

// R5: a UI subtree inside a group is a run boundary within the group
TEST_F(SpriteSortTest, UIInsideAGroupStaysABoundary){
    const Entity g = group("group", 0, 0);
    sprite("front", 4, 0, g);
    ui("panel", g);
    sprite("back", 3, 0, g);
    EXPECT_EQ(order(), (std::vector<std::string>{"group", "front", "panel", "back"}));
}

// R4: Custom Axis sorts renderers of one layer and order by dot(axis, point), larger first; the
// point is a Sprite's bounds' centre (Center) or its pivot (Pivot)
TEST_F(SpriteSortTest, CustomAxisCenterAndPivot){
    sprite("low", 0, 0, NULL_ENTITY, Vector3(0, 0, 0));
    sprite("high", 0, 0, NULL_ENTITY, Vector3(0, 10, 0));
    sprite("middle", 0, 0, NULL_ENTITY, Vector3(0, 5, 0));
    sprite("front-low", 4, 0, NULL_ENTITY, Vector3(0, -50, 0));
    SortingLayers::setTransparencySortMode(TransparencySortMode::CUSTOM_AXIS);
    EXPECT_EQ(order(), (std::vector<std::string>{"high", "middle", "low", "front-low"}));
    // Center: the bounds' centre (high's bounds reach below low's pivot)
    MeshComponent& high = scene.getComponent<MeshComponent>(named["high"]);
    high.worldAABB = AABB(Vector3(-1, -20, 0), Vector3(1, -10, 0));
    EXPECT_EQ(order(), (std::vector<std::string>{"middle", "low", "high", "front-low"}));
    // Pivot: the pivot again
    scene.getComponent<SpriteComponent>(named["high"]).spriteSortPoint = SpriteSortPoint::PIVOT;
    EXPECT_EQ(order(), (std::vector<std::string>{"high", "middle", "low", "front-low"}));
    // another axis: x
    SortingLayers::setTransparencySortAxis(Vector3(1, 0, 0));
    scene.getComponent<Transform>(named["low"]).worldPosition = Vector3(3, 0, 0);
    EXPECT_EQ(order().front(), "low");
    // Custom Axis is on by itself (the axis term), defaults otherwise
    for (const auto& [name, entity] : named)
        if (scene.findComponent<MeshComponent>(entity)) scene.getComponent<MeshComponent>(entity).sortingLayer = 0;
    render().updateSortingActive();
    EXPECT_TRUE(render().isSortingActive());
}

// D2: the key's bits: layer value, Order in Layer + 32768, the axis distance larger first
TEST_F(SpriteSortTest, KeysOrderAsTheSpecSays){
    EXPECT_LT(RenderSystem::makeSortKey(0, 32767, false, 0.0f), RenderSystem::makeSortKey(1, -32768, false, 0.0f));
    EXPECT_LT(RenderSystem::makeSortKey(1, -1, false, 0.0f), RenderSystem::makeSortKey(1, 0, false, 0.0f));
    EXPECT_LT(RenderSystem::makeSortKey(1, 0, true, 5.0f), RenderSystem::makeSortKey(1, 0, true, -5.0f));
    EXPECT_LT(RenderSystem::makeSortKey(1, 0, true, -5.0f), RenderSystem::makeSortKey(1, 0, true, -6.0f));
    EXPECT_EQ(RenderSystem::makeSortKey(1, 0, true, 0.0f), RenderSystem::makeSortKey(1, 0, true, -0.0f));
    // past 16 bits the order clamps
    EXPECT_EQ(RenderSystem::makeSortKey(0, 90000, false, 0.0f), RenderSystem::makeSortKey(0, 32767, false, 0.0f));
}

// R2: the other renderers are sort items with their own fields (a Particle System here)
TEST_F(SpriteSortTest, ParticleSystemsAreSortItems){
    sprite("sprite-front", 4);
    const Entity particles = empty("particles");
    ParticleSystemComponent component;
    component.sortingLayer = 3;
    scene.addComponent<ParticleSystemComponent>(particles, component);
    EXPECT_EQ(order(), (std::vector<std::string>{"particles", "sprite-front"}));
}

// R6: with a Sorting Group in the 3D transparent queue, an item alone and a whole group are units
// of one total order (priority, distance far first, sequence): X (12 m, priority 1), Y (8 m,
// priority 0) and a group G at 10 m once compared Y < X < G < Y, undefined for std::sort
TEST(SpriteSortQueue, GroupsAndItemsAloneAreOneStrictOrder){
    auto alone = [](uint32_t order, float distance, int priority){
        RenderSystem::TransparentRenderData data{};
        data.order = order;
        data.distanceToCamera = distance;
        data.waterSortDistance = distance;
        data.sortPriority = priority;
        data.unitPriority = priority;
        data.unitDistance = distance;
        data.unitSeq = order;
        return data;
    };
    auto member = [](uint32_t order, float distance, uint32_t group, float groupDistance, uint64_t memberKey){
        RenderSystem::TransparentRenderData data{};
        data.order = order;
        data.distanceToCamera = distance;
        data.waterSortDistance = distance;
        data.sortGroup = group;
        data.memberKey = memberKey;
        data.unitDistance = groupDistance;
        data.unitSeq = (1ull << 32) | group;
        return data;
    };
    const std::vector<RenderSystem::TransparentRenderData> items = {
        alone(0, 12.0f, 1),                                       // X
        alone(1, 8.0f, 0),                                        // Y
        member(2, 9.0f, 1, 10.0f, RenderSystem::makeSortKey(1, 0, false, 0.0f)),  // G's members
        member(3, 11.0f, 1, 10.0f, RenderSystem::makeSortKey(0, 0, false, 0.0f)),
    };
    const RenderSystem::TransparentGroupComparison less;
    for (const auto& a : items){
        EXPECT_FALSE(less(a, a));
        for (const auto& b : items){
            if (less(a, b)) EXPECT_FALSE(less(b, a)) << a.order << " " << b.order;
            for (const auto& c : items){
                if (less(a, b) && less(b, c)) EXPECT_TRUE(less(a, c)) << a.order << " < " << b.order << " < " << c.order;
            }
        }
    }
    std::vector<RenderSystem::TransparentRenderData> sorted = items;
    std::sort(sorted.begin(), sorted.end(), less);
    std::vector<uint32_t> order;
    for (const auto& data : sorted) order.push_back(data.order);
    // priority 0 first: the group (10 m) before Y (8 m), its members by their own layer; X last
    EXPECT_EQ(order, (std::vector<uint32_t>{3, 2, 1, 0}));
}
