// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// computeWorldTransform's affine forms (draw-perf spec 6.3): over a hierarchy with rotations, a
// mirrored axis and a UI pivot, the local, model and normal matrices and the world position equal
// the general products they replace (translate * rotate * scale, parent * local, the inverse's
// transpose, the parent matrix times the origin), within a few float steps for compilers that
// fuse multiply-adds differently.

#include "EntityRegistry.h"
#include "component/RectTransformComponent.h"
#include "component/Transform.h"
#include "subsystem/TransformPropagation.h"
#include "EcsTestUtil.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    void expectClose(const Matrix4& actual, const Matrix4& expected, const char* what){
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
        expectClose(transform.localMatrix, local, "local");
        Matrix4 model = local;
        Vector3 worldPosition = origin;
        if (transform.parent != NULL_ENTITY){
            const Transform& parent = registry.getComponent<Transform>(transform.parent);
            model = parent.modelMatrix * local;
            worldPosition = parent.modelMatrix * origin;
        }
        expectClose(transform.modelMatrix, model, "model");
        expectClose(transform.normalMatrix, model.inverse().transpose(), "normal");
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
