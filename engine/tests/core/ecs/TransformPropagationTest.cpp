// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// computeWorldTransform's affine forms (draw-perf spec 6.3): over a hierarchy with rotations, a
// mirrored axis and a UI pivot, the local, model and normal matrices and the world position equal
// the general products they replace (translate * rotate * scale, parent * local, the inverse's
// transpose, the parent matrix times the origin), within a few float steps for compilers that
// fuse multiply-adds differently. S95 REQ-6: the pose computeWorldPose gives on demand is the
// pass's, bit for bit, and costs no allocation.

#include "EntityRegistry.h"
#include "component/RectTransformComponent.h"
#include "component/Transform.h"
#include "subsystem/TransformPropagation.h"
#include "EcsTestUtil.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace TransformPropagationTest_cpp {

using namespace jasperengine;

namespace {

    void transformPropagationExpectClose(const Matrix4& actual, const Matrix4& expected, const char* what){
        for (int c = 0; c < 4; c++){
            for (int r = 0; r < 4; r++){
                const float a = actual.get(c, r);
                const float e = expected.get(c, r);
                const float scale = std::max(1.0f, std::max(std::fabs(a), std::fabs(e)));
                EXPECT_NEAR(a, e, 2e-6f * scale) << what << " [" << c << "][" << r << "]";
            }
        }
    }

    // what computeWorldTransform computed before draw-perf, from the parent's (already checked) values
    void expectGeneral(EntityRegistry& registry, Entity entity){
        const Transform& transform = registry.getComponent<Transform>(entity);
        Vector3 origin = transform.position;
        if (const RectTransformComponent* layout = registry.findComponent<RectTransformComponent>(entity))
            origin += getUIPivotShift(*layout, transform.rotation, transform.scale);
        const Matrix4 local = Matrix4::translateMatrix(origin) * transform.rotation.getRotationMatrix() * Matrix4::scaleMatrix(transform.scale);
        transformPropagationExpectClose(transform.localMatrix, local, "local");
        Matrix4 model = local;
        Vector3 worldPosition = origin;
        if (transform.parent != NULL_ENTITY){
            const Transform& parent = registry.getComponent<Transform>(transform.parent);
            model = parent.modelMatrix * local;
            worldPosition = parent.modelMatrix * origin;
        }
        transformPropagationExpectClose(transform.modelMatrix, model, "model");
        transformPropagationExpectClose(transform.normalMatrix, model.inverse().transpose(), "normal");
        EXPECT_NEAR(transform.worldPosition.x, worldPosition.x, 1e-4f);
        EXPECT_NEAR(transform.worldPosition.y, worldPosition.y, 1e-4f);
        EXPECT_NEAR(transform.worldPosition.z, worldPosition.z, 1e-4f);
    }

}

TEST(TransformPropagation, WorldValuesMatchTheGeneralProducts){
    EntityRegistry registry;
    const Entity root = test::makeTransformEntity(registry);
    const Entity child = test::makeTransformEntity(registry, root);
    const Entity grandchild = test::makeTransformEntity(registry, child);

    Transform& r = registry.getComponent<Transform>(root);
    r.position = Vector3(3.0f, -2.0f, 10.0f);
    r.rotation = Quaternion(35.0f, Vector3(0.2f, 1.0f, 0.1f).normalize());
    r.scale = Vector3(2.0f, 1.5f, 0.75f);
    r.needUpdate = true;

    Transform& c = registry.getComponent<Transform>(child);
    c.position = Vector3(-1.0f, 4.0f, 0.5f);
    c.rotation = Quaternion(-70.0f, Vector3(1.0f, 0.0f, 0.3f).normalize());
    c.scale = Vector3(-1.0f, 1.0f, 1.0f);   // a mirrored axis
    c.needUpdate = true;

    // a UI element growing around its pivot
    registry.addComponent<RectTransformComponent>(grandchild);
    RectTransformComponent& layout = registry.getComponent<RectTransformComponent>(grandchild);
    layout.width = 120;
    layout.height = 40;
    layout.pivot = Vector2(0.5f, 0.5f);
    Transform& g = registry.getComponent<Transform>(grandchild);
    g.position = Vector3(7.0f, 0.0f, -3.0f);
    g.rotation = Quaternion(12.0f, Vector3(0.0f, 0.0f, 1.0f));
    g.scale = Vector3(0.5f, 3.0f, 1.0f);
    g.needUpdate = true;

    updateWorldTransforms(registry);

    expectGeneral(registry, root);
    expectGeneral(registry, child);
    expectGeneral(registry, grandchild);
    EXPECT_LT(registry.getComponent<Transform>(child).modelMatrix.determinant3x3(), 0.0f) << "the mirrored child";
}

namespace {

    // the pass's flags cleared, as RenderSystem::update leaves them
    void settlePoses(EntityRegistry& registry){
        updateWorldTransforms(registry);
        auto transforms = registry.getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++){
            transforms->getComponentFromIndex(i).needUpdate = false;
        }
    }

    void expectPassPose(const TransformPose& pose, const Transform& transform, const char* what){
        EXPECT_EQ(std::memcmp(&pose.position, &transform.worldPosition, sizeof(Vector3)), 0) << what << " position";
        EXPECT_EQ(std::memcmp(&pose.rotation, &transform.worldRotation, sizeof(Quaternion)), 0) << what << " rotation";
        EXPECT_EQ(std::memcmp(&pose.scale, &transform.worldScale, sizeof(Vector3)), 0) << what << " scale";
        EXPECT_EQ(std::memcmp(&pose.modelMatrix, &transform.modelMatrix, sizeof(Matrix4)), 0) << what << " model";
    }

}

// S95 REQ-6, D-4: computeWorldPose gives the pass's bits, for a clean Transform (its stored
// values), under a moved ancestor (composed down from the last clean one, a UI pivot and a
// mirrored axis included) and for a chain deeper than it keeps (40 > 32), and writes nothing
TEST(TransformPropagation, WorldPoseOnDemandMatchesThePass){
    EntityRegistry registry;
    std::vector<Entity> chain;
    Entity parent = NULL_ENTITY;
    for (int d = 0; d < 40; d++){
        parent = test::makeTransformEntity(registry, parent);
        Transform& transform = registry.getComponent<Transform>(parent);
        transform.position = Vector3(0.5f * (float)d, 1.0f, -0.25f);
        transform.rotation = Quaternion(7.0f * (float)d, Vector3(0.2f, 1.0f, 0.1f).normalize());
        transform.scale = (d == 5) ? Vector3(-1.0f, 1.1f, 0.9f) : Vector3(1.02f, 0.98f, 1.0f);
        chain.push_back(parent);
    }
    registry.addComponent<RectTransformComponent>(chain[20]);
    RectTransformComponent& layout = registry.getComponent<RectTransformComponent>(chain[20]);
    layout.width = 64;
    layout.height = 32;
    layout.pivot = Vector2(0.25f, 0.75f);
    settlePoses(registry);

    const Entity leaf = chain.back();
    expectPassPose(computeWorldPose(registry, leaf), registry.getComponent<Transform>(leaf), "clean");

    // moved: near the root (a chain deeper than kept), and in the middle (pivot below)
    for (const size_t moved : {size_t(1), size_t(18)}){
        Transform& transform = registry.getComponent<Transform>(chain[moved]);
        transform.position += Vector3(3.0f, -2.0f, 1.0f);
        transform.rotation = transform.rotation * Quaternion(25.0f, Vector3(1, 0, 0));
        transform.needUpdate = true;
        const Transform before = registry.getComponent<Transform>(leaf);
        std::vector<TransformPose> seen;
        for (const Entity entity : chain) seen.push_back(computeWorldPose(registry, entity));

        const Transform& after = registry.getComponent<Transform>(leaf);
        EXPECT_EQ(std::memcmp(&before.modelMatrix, &after.modelMatrix, sizeof(Matrix4)), 0) << "nothing written";
        EXPECT_FALSE(after.needUpdate) << "no flag set";
        EXPECT_TRUE(registry.getComponent<Transform>(chain[moved]).needUpdate) << "no flag cleared";

        settlePoses(registry);
        for (size_t i = 0; i < chain.size(); i++){
            SCOPED_TRACE("moved " + std::to_string(moved) + ", depth " + std::to_string(i));
            expectPassPose(seen[i], registry.getComponent<Transform>(chain[i]), "after the move");
        }
    }
}

// S95 REQ-35: no allocation, clean or dirty
TEST(TransformPropagation, WorldPoseOnDemandDoesNotAllocate){
    if (!testsupport::countsAllocations()) GTEST_SKIP() << "this build cannot count allocations";
    EntityRegistry registry;
    Entity parent = NULL_ENTITY;
    Entity root = NULL_ENTITY;
    for (int d = 0; d < 40; d++){
        parent = test::makeTransformEntity(registry, parent);
        if (d == 0) root = parent;
    }
    settlePoses(registry);
    float sum = 0.0f;
    testsupport::ThreadAllocationCount clean;
    sum += computeWorldPose(registry, parent).position.x;
    EXPECT_EQ(clean.count(), 0u);
    registry.getComponent<Transform>(root).needUpdate = true;
    testsupport::ThreadAllocationCount dirty;
    sum += computeWorldPose(registry, parent).position.x;
    EXPECT_EQ(dirty.count(), 0u);
    EXPECT_EQ(sum, 0.0f);
}

}  // namespace TransformPropagationTest_cpp
