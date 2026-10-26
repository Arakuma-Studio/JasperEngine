// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Typed views: the Read/Write/Optional/Without matrix, parent-before-child order when the
// Transforms drive, and forEachPage covering what each visits.

#include "EntityRegistry.h"
#include "EcsTestUtil.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <vector>

using namespace jasperengine;
using jasperengine::test::makeTransformEntity;

TEST(ViewTest, ReadWriteOptionalWithoutMatrix){
    EntityRegistry registry;
    // a: Transform + Mesh; b: Transform + Mesh + UI; c: Transform + UI; d: Mesh only
    const Entity a = makeTransformEntity(registry);
    const Entity b = makeTransformEntity(registry);
    const Entity c = makeTransformEntity(registry);
    const Entity d = registry.createEntity();
    registry.addComponent<MeshComponent>(a);
    registry.addComponent<MeshComponent>(b);
    registry.addComponent<UIComponent>(b);
    registry.addComponent<UIComponent>(c);
    registry.addComponent<MeshComponent>(d);

    std::vector<Entity> both;
    registry.view<Read<Transform>, Write<MeshComponent>>().each([&](Entity e, const Transform&, MeshComponent& mesh){
        mesh.enabled = false;
        both.push_back(e);
    });
    EXPECT_EQ(both, (std::vector<Entity>{a, b}));
    EXPECT_FALSE(registry.getComponent<MeshComponent>(a).enabled);
    EXPECT_TRUE(registry.getComponent<MeshComponent>(d).enabled);

    std::vector<Entity> withoutUI;
    registry.view<Read<Transform>, Without<UIComponent>>().each([&](Entity e, const Transform&){ withoutUI.push_back(e); });
    EXPECT_EQ(withoutUI, (std::vector<Entity>{a}));

    std::vector<std::pair<Entity, bool>> optional;
    registry.view<Read<Transform>, Optional<UIComponent>>().each([&](Entity e, const Transform&, UIComponent* ui){
        optional.push_back({e, ui != nullptr});
    });
    EXPECT_EQ(optional, (std::vector<std::pair<Entity, bool>>{{a, false}, {b, true}, {c, true}}));

    // the first Read/Write type drives: the Mesh array's order, d included
    std::vector<Entity> meshes;
    registry.view<Optional<Transform>, Read<MeshComponent>>().each([&](Entity e, Transform* transform, const MeshComponent&){
        EXPECT_EQ(transform == nullptr, e == d);
        meshes.push_back(e);
    });
    EXPECT_EQ(meshes, (std::vector<Entity>{a, b, d}));

    EXPECT_EQ((registry.view<Read<MeshComponent>, Read<UIComponent>>().count()), 1u);
}

TEST(ViewTest, TransformDrivenIsParentBeforeChild){
    EntityRegistry registry;
    // children created before their parents get them, then moved under them
    const Entity child = makeTransformEntity(registry);
    const Entity grandchild = makeTransformEntity(registry);
    const Entity parent = makeTransformEntity(registry);
    registry.addEntityChild(parent, child, false);
    registry.addEntityChild(child, grandchild, false);

    std::vector<Entity> order;
    registry.view<Read<Transform>>().each([&](Entity e, const Transform& transform){
        if (transform.parent != NULL_ENTITY){
            EXPECT_NE(std::find(order.begin(), order.end(), transform.parent), order.end()) << "parent after " << e;
        }
        order.push_back(e);
    });
    EXPECT_EQ(order, (std::vector<Entity>{parent, child, grandchild}));

    // a Hierarchy type drives in the same order, the component added late included
    registry.addComponent<MeshComponent>(grandchild);
    registry.addComponent<MeshComponent>(parent);
    std::vector<Entity> meshes;
    registry.view<Read<MeshComponent>>().each([&](Entity e, const MeshComponent&){ meshes.push_back(e); });
    EXPECT_EQ(meshes, (std::vector<Entity>{parent, grandchild}));
}

TEST(ViewTest, ForEachPageCoversEach){
    EntityRegistry registry;
    std::vector<Entity> entities;
    for (int i = 0; i < 1000; i++){
        const Entity entity = makeTransformEntity(registry);
        if (i % 3 == 0){
            registry.addComponent<UIComponent>(entity);
        }
        entities.push_back(entity);
    }
    for (int i = 0; i < 1000; i += 7){
        registry.destroyEntity(entities[i]);
    }

    std::vector<Entity> each;
    registry.view<Read<Transform>, Without<UIComponent>>().each([&](Entity e, const Transform&){ each.push_back(e); });

    std::vector<Entity> paged;
    size_t pages = 0;
    registry.view<Read<Transform>, Without<UIComponent>>().forEachPage([&](const auto& page){
        EXPECT_LE(page.size(), ComponentArray<Transform>::pageCapacity());
        pages++;
        page.each([&](Entity e, const Transform&){ paged.push_back(e); });
    });
    EXPECT_EQ(paged, each);
    EXPECT_GT(pages, 1u);
    EXPECT_FALSE(each.empty());
}

TEST(ViewTest, ChangedAndAddedFilters){
    EntityRegistry registry;
    const Entity a = makeTransformEntity(registry);
    const Entity b = makeTransformEntity(registry);
    const Tick gate = registry.changeTick();
    const Entity c = makeTransformEntity(registry);
    registry.markChanged<Transform>(a);

    std::set<Entity> changed, added;
    registry.view<Read<Transform>>().changedSince(gate).each([&](Entity e, const Transform&){ changed.insert(e); });
    registry.view<Read<Transform>>().addedSince(gate).each([&](Entity e, const Transform&){ added.insert(e); });
    EXPECT_EQ(changed, (std::set<Entity>{a, c}));
    EXPECT_EQ(added, (std::set<Entity>{c}));
    (void)b;
}
