// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Bounds hold the vertices only (docs/superpowers/specs/2026-11-04-render-correctness-design.md
// 6.1). They were seeded with AABB::ZERO, a finite box at the origin that every merge kept, so a
// mesh or a UI polygon away from its own origin had bounds stretched back to it: looser culling,
// oversized selection and picking boxes, a wrong water level. A mesh without float positions
// keeps AABB::ZERO, the "not computed" value its users test for.

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/PolygonComponent.h"
#include "component/RectTransformComponent.h"
#include "component/UIComponent.h"
#include "object/MeshPolygon.h"
#include "object/ui/Polygon.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/UISystem.h"

#include <gtest/gtest.h>

using namespace jasperengine;

namespace {

    // a triangle of float positions in a submesh of its own
    Entity makeBoundsTriangle(Scene& scene, const Vector3& a, const Vector3& b, const Vector3& c){
        const Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity);
        scene.addComponent<MeshComponent>(entity);
        MeshComponent& mesh = scene.getComponent<MeshComponent>(entity);
        mesh.buffer.addAttribute(AttributeType::POSITION, 3);
        Attribute* position = mesh.buffer.getAttribute(AttributeType::POSITION);
        mesh.buffer.addVector3(position, a);
        mesh.buffer.addVector3(position, b);
        mesh.buffer.addVector3(position, c);
        mesh.numSubmeshes = 1;
        mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
        mesh.submeshes[0].attributes[AttributeType::POSITION] = *position;
        mesh.submeshes[0].attributes[AttributeType::POSITION].setBufferName("vertices");
        return entity;
    }

    void expectBoundsVector(const Vector3& actual, const Vector3& expected, const char* what){
        EXPECT_NEAR(actual.x, expected.x, 1e-5f) << what;
        EXPECT_NEAR(actual.y, expected.y, 1e-5f) << what;
        EXPECT_NEAR(actual.z, expected.z, 1e-5f) << what;
    }

}

TEST(MeshBounds, OffsetMeshBoundsHugTheVertices){
    Scene scene;
    const Entity entity = makeBoundsTriangle(scene, Vector3(10, 10, 10), Vector3(11, 10, 10), Vector3(10, 11, 11));
    MeshComponent& mesh = scene.getComponent<MeshComponent>(entity);
    scene.getSystem<MeshSystem>()->calculateMeshAABB(mesh);

    expectBoundsVector(mesh.verticesAABB.getMinimum(), Vector3(10, 10, 10), "minimum");
    expectBoundsVector(mesh.verticesAABB.getMaximum(), Vector3(11, 11, 11), "maximum");
    EXPECT_EQ(mesh.aabb, mesh.verticesAABB);
}

TEST(MeshBounds, MeshAroundItsOriginIsUnchanged){
    Scene scene;
    const Entity entity = makeBoundsTriangle(scene, Vector3(-1, -2, -3), Vector3(1, 0, 0), Vector3(0, 2, 3));
    MeshComponent& mesh = scene.getComponent<MeshComponent>(entity);
    scene.getSystem<MeshSystem>()->calculateMeshAABB(mesh);

    expectBoundsVector(mesh.verticesAABB.getMinimum(), Vector3(-1, -2, -3), "minimum");
    expectBoundsVector(mesh.verticesAABB.getMaximum(), Vector3(1, 2, 3), "maximum");
}

TEST(MeshBounds, EmptyMeshKeepsZero){
    Scene scene;
    const Entity entity = scene.createEntity();
    scene.addComponent<Transform>(entity);
    scene.addComponent<MeshComponent>(entity);
    MeshComponent& mesh = scene.getComponent<MeshComponent>(entity);
    scene.getSystem<MeshSystem>()->calculateMeshAABB(mesh);

    EXPECT_EQ(mesh.verticesAABB, AABB::ZERO);
    EXPECT_EQ(mesh.aabb, AABB::ZERO);
    EXPECT_FALSE(mesh.verticesAABB.isNull());
}

TEST(MeshBounds, UiPolygonBoundsHugTheVertices){
    Scene scene;
    Polygon polygon(&scene);
    polygon.addVertex(5, 2);
    polygon.addVertex(9, 2);
    polygon.addVertex(9, 6);

    PolygonComponent& component = scene.getComponent<PolygonComponent>(polygon.getEntity());
    UIComponent& ui = scene.getComponent<UIComponent>(polygon.getEntity());
    RectTransformComponent& layout = scene.getComponent<RectTransformComponent>(polygon.getEntity());
    // no camera to flip the polygon for
    ui.automaticFlipY = false;
    scene.getSystem<UISystem>()->createOrUpdatePolygon(component, ui, layout);

    ASSERT_FALSE(ui.aabb.isNull());
    EXPECT_NEAR(ui.aabb.getMinimum().x, 5.0f, 1e-5f);
    EXPECT_NEAR(ui.aabb.getMinimum().y, 2.0f, 1e-5f);
    EXPECT_NEAR(ui.aabb.getMaximum().x, 9.0f, 1e-5f);
    EXPECT_NEAR(ui.aabb.getMaximum().y, 6.0f, 1e-5f);
}

// the texture coordinates and size of a polygon measured its extent from 0 when it lay left of
// or below its origin (numeric_limits<float>::min() is the smallest positive float)
TEST(MeshBounds, PolygonLeftOfItsOriginMeasuresItsOwnExtent){
    Scene scene;
    Polygon polygon(&scene);
    polygon.addVertex(-9, -6);
    polygon.addVertex(-5, -6);
    polygon.addVertex(-5, -2);

    PolygonComponent& component = scene.getComponent<PolygonComponent>(polygon.getEntity());
    UIComponent& ui = scene.getComponent<UIComponent>(polygon.getEntity());
    RectTransformComponent& layout = scene.getComponent<RectTransformComponent>(polygon.getEntity());
    // no camera to flip the polygon for
    ui.automaticFlipY = false;
    scene.getSystem<UISystem>()->createOrUpdatePolygon(component, ui, layout);
    EXPECT_EQ(layout.width, 4u);
    EXPECT_EQ(layout.height, 4u);
    EXPECT_NEAR(ui.aabb.getMaximum().x, -5.0f, 1e-5f);

    MeshPolygon meshPolygon(&scene);
    meshPolygon.addVertex(-9, -6);
    meshPolygon.addVertex(-5, -6);
    meshPolygon.addVertex(-5, -2);
    MeshPolygonComponent& meshComponent = scene.getComponent<MeshPolygonComponent>(meshPolygon.getEntity());
    MeshComponent& mesh = scene.getComponent<MeshComponent>(meshPolygon.getEntity());
    meshComponent.automaticFlipY = false;
    ASSERT_TRUE(meshPolygon.createPolygon());
    EXPECT_EQ(meshComponent.width, 4u);
    EXPECT_EQ(meshComponent.height, 4u);
    expectBoundsVector(mesh.verticesAABB.getMaximum(), Vector3(-5, -2, 0), "mesh polygon maximum");
}
