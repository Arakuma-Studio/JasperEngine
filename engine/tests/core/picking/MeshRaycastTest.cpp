// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// MeshRaycast (docs/specs/S26-scene-view.md R3-R6, R35, AC3-AC5, AC18): triangles are hit from
// the side the renderer draws, every pickable kind answers by its own test, posed meshes are hit
// where they are drawn, and the distances follow Physics.raycast's semantics.

#include "picking/MeshRaycast.h"
#include "picking/MeshPose.h"
#include "picking/PickCache.h"
#include "action/ParticleSystem.h"
#include "component/InstancedMeshComponent.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "object/Camera.h"
#include "object/Mesh.h"
#include "object/Points.h"
#include "object/Primitive.h"
#include "object/Sprite.h"
#include "object/Terrain.h"
#include "object/Tilemap.h"
#include "picking/PickingTestUtil.h"
#include "util/Angle.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // A two-part "character": a body box around the origin on bone 0 and an arm box to its
    // right (x in [1, 3]) on bone 1, skinned in mesh space
    Entity makeSkinnedArm(Scene& scene){
        const Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity);
        scene.addComponent<MeshComponent>(entity);
        MeshComponent& mesh = scene.getComponent<MeshComponent>(entity);
        mesh.buffer.addAttribute(AttributeType::POSITION, 3);
        mesh.buffer.addAttribute(AttributeType::BONEIDS, 4);
        mesh.buffer.addAttribute(AttributeType::BONEWEIGHTS, 4);
        Attribute* position = mesh.buffer.getAttribute(AttributeType::POSITION);
        Attribute* ids = mesh.buffer.getAttribute(AttributeType::BONEIDS);
        Attribute* weights = mesh.buffer.getAttribute(AttributeType::BONEWEIGHTS);
        Attribute* index = mesh.indices.getAttribute(AttributeType::INDEX);

        auto box = [&](Vector3 mn, Vector3 mx, float bone){
            const uint16_t base = (uint16_t)position->getCount();
            for (int corner = 0; corner < 8; corner++){
                mesh.buffer.addVector3(position, Vector3((corner & 1) ? mx.x : mn.x, (corner & 2) ? mx.y : mn.y, (corner & 4) ? mx.z : mn.z));
                mesh.buffer.addVector4(ids, Vector4(bone, 0, 0, 0));
                mesh.buffer.addVector4(weights, Vector4(1, 0, 0, 0));
            }
            // outward counter-clockwise faces
            const uint16_t faces[36] = {0, 2, 1, 1, 2, 3,  4, 5, 6, 5, 7, 6,  0, 1, 4, 1, 5, 4,
                                        2, 6, 3, 3, 6, 7,  0, 4, 2, 2, 4, 6,  1, 3, 5, 3, 7, 5};
            for (uint16_t f : faces){
                mesh.indices.addUInt16(index, (uint16_t)(base + f));
            }
        };
        box(Vector3(-1, -1, -1), Vector3(1, 1, 1), 0.0f);
        box(Vector3(1, -0.25f, -0.25f), Vector3(3, 0.25f, 0.25f), 1.0f);

        mesh.numSubmeshes = 1;
        Submesh& submesh = mesh.submeshes[0];
        submesh.primitiveType = PrimitiveType::TRIANGLES;
        submesh.hasSkinning = true;
        submesh.attributes[AttributeType::POSITION] = *position;
        submesh.attributes[AttributeType::POSITION].setBufferName("vertices");
        submesh.attributes[AttributeType::BONEIDS] = *ids;
        submesh.attributes[AttributeType::BONEIDS].setBufferName("vertices");
        submesh.attributes[AttributeType::BONEWEIGHTS] = *weights;
        submesh.attributes[AttributeType::BONEWEIGHTS].setBufferName("vertices");
        mesh.bonesMatrix[0] = Matrix4();
        mesh.bonesMatrix[1] = Matrix4();
        scene.getSystem<MeshSystem>()->calculateMeshAABB(mesh);
        return entity;
    }

    // the arm raised: bone 1 turns 90 degrees about z around the shoulder (1, 0, 0)
    void raiseArm(Scene& scene, Entity entity){
        MeshComponent& mesh = scene.getComponent<MeshComponent>(entity);
        mesh.bonesMatrix[1] = Matrix4::translateMatrix(Vector3(1, 0, 0)) * Matrix4::rotateZMatrix(Angle::degToDefault(90.0f)) *
                              Matrix4::translateMatrix(Vector3(-1, 0, 0));
        mesh.needUpdateAABB = true;
    }

}

TEST(MeshRaycast, BackfaceCulledRoom){
    Scene scene;
    Primitive room(&scene);
    room.createBox(10, 10, 10);
    Primitive box(&scene);
    box.createBox(1, 1, 1);
    box.setPosition(0, 0, -8);
    settleScene(scene);

    // from inside the room, through its wall (a back face), onto the box behind it
    const Ray ray(Vector3(0, 0, 0), Vector3(0, 0, -20));
    MeshRaycastHit hit = MeshRaycast::raycast(&scene, ray);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.entity, box.getEntity());
    EXPECT_NEAR(hit.distance * 20.0f, 7.5f, 1e-4f);

    // a double-sided room: its wall
    room.setFaceCulling(false);
    hit = MeshRaycast::raycast(&scene, ray);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.entity, room.getEntity());
    EXPECT_NEAR(hit.distance * 20.0f, 5.0f, 1e-4f);
    // the normal faces the ray
    EXPECT_GT(hit.normal.z, 0.9f);

    // culled again, and hitBackfaces asks for every side
    room.setFaceCulling(true);
    hit = MeshRaycast::raycast(&scene, ray);
    EXPECT_EQ(hit.entity, box.getEntity());
    MeshRaycastOptions options;
    options.hitBackfaces = true;
    hit = MeshRaycast::raycast(&scene, ray, LAYER_MASK_DEFAULT_RAYCAST, options);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.entity, room.getEntity());

    // a mirroring scale turns the faces inside out: the room's inside is then drawn
    room.setScale(Vector3(-1, 1, 1));
    settleScene(scene);
    hit = MeshRaycast::raycast(&scene, ray);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.entity, room.getEntity());
}

TEST(MeshRaycast, Kinds){
    Scene scene;
    // sprites and terrains read the scene's camera when they build
    Camera camera(&scene);
    camera.setType(CameraType::CAMERA_PERSPECTIVE);
    camera.activate();

    // a sprite turned 45 degrees: its bounds' corners are a gap
    Sprite sprite(&scene);
    sprite.setSize(10, 10);
    sprite.setPivotPreset(PivotPreset::CENTER);
    sprite.setPosition(-30, 0, 0);
    sprite.setRotation(0, 0, 45);

    // a tilemap with two tiles and a hole between them
    Tilemap tilemap(&scene);
    tilemap.addRect(0, 0, 16, 16);
    tilemap.addTile(0, Vector2(0, 0), 4, 4);
    tilemap.addTile(0, Vector2(8, 0), 4, 4);
    tilemap.setPosition(30, 0, 0);

    // an instanced mesh: instance 7 is the one in the ray
    Primitive instanced(&scene);
    instanced.createBox(1, 1, 1);
    instanced.createInstancedMesh();
    instanced.setPosition(0, 30, 0);

    Terrain terrain(&scene);
    terrain.setPosition(0, -50, 0);

    Points points(&scene);
    points.addPoint(Vector3(-1, -1, 0));
    points.addPoint(Vector3(1, 1, 0));
    points.setPosition(0, 0, 40);

    settleScene(scene);
    {
        InstancedMeshComponent& instmesh = scene.getComponent<InstancedMeshComponent>(instanced.getEntity());
        for (int i = 0; i < 10; i++){
            InstanceData data;
            data.position = Vector3((float)i * 3.0f, 0, 0);
            instmesh.instances.push_back(data);
        }
        instmesh.needUpdateInstances = true;
    }
    settleScene(scene);

    // sprite: the centre hits, the corner of its bounds misses
    MeshRaycastHit hit = MeshRaycast::raycast(&scene, Vector3(-30, 0, 50), Vector3(0, 0, -1), 100.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.entity, sprite.getEntity());
    EXPECT_EQ(hit.kind, PickKind::MESH);
    hit = MeshRaycast::raycast(&scene, Vector3(-30 + 6.0f, 6.0f, 50), Vector3(0, 0, -1), 100.0f);
    EXPECT_FALSE(hit.hit && hit.entity == sprite.getEntity());

    // tilemap: a tile hits, the hole misses
    hit = MeshRaycast::raycast(&scene, Vector3(30 + 2, 2, 50), Vector3(0, 0, -1), 100.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.entity, tilemap.getEntity());
    hit = MeshRaycast::raycast(&scene, Vector3(30 + 6, 2, 50), Vector3(0, 0, -1), 100.0f);
    EXPECT_FALSE(hit.hit && hit.entity == tilemap.getEntity());

    // instance 7
    hit = MeshRaycast::raycast(&scene, Vector3(21, 30, 50), Vector3(0, 0, -1), 100.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.entity, instanced.getEntity());
    EXPECT_EQ(hit.kind, PickKind::INSTANCE);
    EXPECT_EQ(hit.instance, 7);
    EXPECT_NEAR(hit.distance, 49.5f, 1e-4f);

    // terrain: as raycastTerrainSurface finds it
    const Ray down(Vector3(12, 0, 7), Vector3(0, -100, 0));
    hit = MeshRaycast::raycast(&scene, down, LAYER_MASK_EVERYTHING);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.entity, terrain.getEntity());
    EXPECT_EQ(hit.kind, PickKind::TERRAIN);
    Vector3 surface;
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->raycastTerrainSurface(down, scene.getComponent<TerrainComponent>(terrain.getEntity()),
                                                                     scene.getComponent<Transform>(terrain.getEntity()), surface));
    EXPECT_NEAR(hit.point.x, surface.x, 1e-4f);
    EXPECT_NEAR(hit.point.y, surface.y, 1e-4f);
    EXPECT_NEAR(hit.point.z, surface.z, 1e-4f);
    EXPECT_GT(hit.normal.y, 0.99f);

    // points: their bounds
    hit = MeshRaycast::raycast(&scene, Vector3(0.2f, 0.3f, 60), Vector3(0, 0, -1), 100.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.entity, points.getEntity());
    EXPECT_EQ(hit.kind, PickKind::BOUNDS);

    // a particle system drawing instances: the system's bounds, never one particle
    ParticleSystem particles(&scene);
    particles.setTarget(instanced.getEntity());
    scene.getPickBvh().markStructureChanged();
    hit = MeshRaycast::raycast(&scene, Vector3(21, 30, 50), Vector3(0, 0, -1), 100.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.entity, instanced.getEntity());
    EXPECT_EQ(hit.kind, PickKind::BOUNDS);
    EXPECT_EQ(hit.instance, -1);

    // kinds filter
    MeshRaycastOptions onlyMeshes;
    onlyMeshes.kinds = MeshRaycast::MESH;
    hit = MeshRaycast::raycast(&scene, Vector3(0.2f, 0.3f, 60), Vector3(0, 0, -1), 100.0f, LAYER_MASK_EVERYTHING, onlyMeshes);
    EXPECT_FALSE(hit.hit);
}

TEST(MeshRaycast, Lod0TrianglesWhileLod2IsDrawn){
    // a quad whose LOD2 would be a single triangle covering only half of it
    Scene scene;
    Primitive quad(&scene);
    quad.createPlane(2, 2);
    settleScene(scene);
    MeshComponent& mesh = scene.getComponent<MeshComponent>(quad.getEntity());
    mesh.numLods = 3;
    Submesh& submesh = mesh.submeshes[0];
    submesh.lodIndices.setName("lod-indices");
    std::vector<uint32_t> half = {0, 1, 2};
    submesh.lodIndices.setData(reinterpret_cast<unsigned char*>(half.data()), half.size() * sizeof(uint32_t));
    // both halves of the quad still pick: LOD0's two triangles
    int hits = 0;
    for (float x : {-0.5f, 0.5f}){
        for (float z : {-0.5f, 0.5f}){
            const MeshRaycastHit hit = MeshRaycast::raycast(&scene, Vector3(x, 5, z), Vector3(0, -1, 0), 10.0f);
            if (hit.hit && hit.entity == quad.getEntity()){
                hits++;
            }
        }
    }
    EXPECT_EQ(hits, 4);
}

TEST(MeshRaycast, SkinnedArmRaised){
    Scene scene;
    const Entity character = makeSkinnedArm(scene);
    settleScene(scene);
    ASSERT_TRUE(picking::isPosed(scene.getComponent<MeshComponent>(character)));

    // bind pose: the arm lies along +x
    const Ray atBindArm(Vector3(2, 10, 0), Vector3(0, -20, 0));
    EXPECT_TRUE(MeshRaycast::raycast(&scene, atBindArm).hit);

    raiseArm(scene, character);
    settleScene(scene);
    // raised: hit where it is drawn now (the middle of the arm through its bone), missed
    // where it was
    const Vector3 middle = scene.getComponent<MeshComponent>(character).bonesMatrix[1] * Vector3(2, 0, 0);
    const Ray atRaisedArm(middle + Vector3(0, 0, 10), Vector3(0, 0, -20));
    MeshRaycastHit hit = MeshRaycast::raycast(&scene, atRaisedArm);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.entity, character);
    EXPECT_NEAR(hit.point.z, 0.25f, 1e-4f);
    hit = MeshRaycast::raycast(&scene, atBindArm);
    EXPECT_FALSE(hit.hit);

    // the same pose again: the posed copy is reused (the cache's tree count stays)
    const uint64_t builds = PickCache::get().stats().builds;
    hit = MeshRaycast::raycast(&scene, atRaisedArm);
    EXPECT_TRUE(hit.hit);
    EXPECT_EQ(PickCache::get().stats().builds, builds);
}

TEST(MeshRaycast, PoseVertexMatchesShaderMath){
    // two bones blended half and half, a morph target first: getMorphPosition, then
    // getBoneTransform and getSkinPosition with the divide by w
    picking::VertexPoseInput input;
    input.position = Vector3(1, 2, 3);
    input.morphTargets[0] = Vector3(0.5f, 0, 0);
    input.morphCount = 1;
    input.skinned = true;
    input.boneIds[0] = 0;
    input.boneIds[1] = 1;
    input.boneWeights[0] = 0.5f;
    input.boneWeights[1] = 0.5f;
    const Matrix4 bones[2] = {Matrix4::translateMatrix(Vector3(10, 0, 0)), Matrix4::translateMatrix(Vector3(0, 20, 0))};
    const float morphWeights[MAX_MORPHTARGETS] = {2.0f};
    picking::PoseParameters parameters;
    parameters.bones = bones;
    parameters.morphWeights = morphWeights;
    Matrix4 boneTransform;
    const Vector3 posed = picking::poseVertex(input, parameters, &boneTransform);
    // morphed: (2, 2, 3); half of +10 x and half of +20 y
    EXPECT_NEAR(posed.x, 7.0f, 1e-5f);
    EXPECT_NEAR(posed.y, 12.0f, 1e-5f);
    EXPECT_NEAR(posed.z, 3.0f, 1e-5f);
    EXPECT_NEAR(boneTransform.get(3, 3), 1.0f, 1e-6f);

    // weights that do not sum to 1: the divide by w normalizes them, as the shader does
    input.boneWeights[0] = 0.25f;
    input.boneWeights[1] = 0.25f;
    const Vector3 light = picking::poseVertex(input, parameters);
    EXPECT_NEAR(light.x, 7.0f, 1e-5f);
    EXPECT_NEAR(light.y, 12.0f, 1e-5f);

    // normAdjust scales ids and weights
    input.boneWeights[0] = 0.5f;
    input.boneWeights[1] = 0.5f;
    parameters.normAdjustWeight = 2.0f;
    const Vector3 scaled = picking::poseVertex(input, parameters, &boneTransform);
    EXPECT_NEAR(scaled.x, 7.0f, 1e-5f);
    EXPECT_NEAR(boneTransform.get(3, 3), 2.0f, 1e-6f);
    input.boneIds[1] = 0.5f;
    parameters.normAdjustJoint = 2.0f;
    parameters.normAdjustWeight = 1.0f;
    EXPECT_NEAR(picking::poseVertex(input, parameters).y, 12.0f, 1e-5f);
    // a bone index the mesh does not have adds nothing
    parameters.normAdjustJoint = 1.0f;
    input.boneIds[1] = 9;
    input.boneWeights[0] = 1.0f;
    input.boneWeights[1] = 1.0f;
    const Vector3 single = picking::poseVertex(input, parameters);
    EXPECT_NEAR(single.x, 12.0f, 1e-5f);
}

TEST(MeshRaycast, DistanceSemanticsAndAll){
    Scene scene;
    Primitive nearBox(&scene);
    nearBox.createBox(1, 1, 1);
    nearBox.setPosition(0, 0, -3);
    Primitive farBox(&scene);
    farBox.createBox(1, 1, 1);
    farBox.setPosition(0, 0, -6);
    settleScene(scene);

    // a Ray: the fraction of its direction; origin, direction, maxDistance: world units
    MeshRaycastHit hit = MeshRaycast::raycast(&scene, Ray(Vector3(0, 0, 0), Vector3(0, 0, -10)));
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.distance, 0.25f, 1e-5f);
    hit = MeshRaycast::raycast(&scene, Vector3(0, 0, 0), Vector3(0, 0, -5), 10.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.distance, 2.5f, 1e-5f);
    EXPECT_NEAR(hit.point.z, -2.5f, 1e-5f);
    // the range ends before it
    EXPECT_FALSE(MeshRaycast::raycast(&scene, Vector3(0, 0, 0), Vector3(0, 0, -1), 2.0f).hit);
    EXPECT_FALSE(MeshRaycast::raycast(&scene, Ray(Vector3(0, 0, 0), Vector3(0, 0, -2))).hit);

    // all: nearest first, one per object
    const std::vector<MeshRaycastHit> all = MeshRaycast::raycastAll(&scene, Vector3(0, 0, 0), Vector3(0, 0, -1), 100.0f);
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].entity, nearBox.getEntity());
    EXPECT_EQ(all[1].entity, farBox.getEntity());
    EXPECT_NEAR(all[1].distance, 5.5f, 1e-5f);

    // ignore
    MeshRaycastOptions options;
    options.ignore = {nearBox.getEntity()};
    hit = MeshRaycast::raycast(&scene, Vector3(0, 0, 0), Vector3(0, 0, -1), 100.0f, LAYER_MASK_DEFAULT_RAYCAST, options);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.entity, farBox.getEntity());

    // one entity
    hit = MeshRaycast::raycastEntity(&scene, farBox.getEntity(), Ray(Vector3(0, 0, 0), Vector3(0, 0, -100)));
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.entity, farBox.getEntity());

    // what is hidden or inactive is not hit unless asked for
    nearBox.setVisible(false);
    settleScene(scene);
    hit = MeshRaycast::raycast(&scene, Vector3(0, 0, 0), Vector3(0, 0, -1), 100.0f);
    EXPECT_EQ(hit.entity, farBox.getEntity());
    MeshRaycastOptions inactive;
    inactive.includeInactive = true;
    hit = MeshRaycast::raycast(&scene, Vector3(0, 0, 0), Vector3(0, 0, -1), 100.0f, LAYER_MASK_DEFAULT_RAYCAST, inactive);
    EXPECT_EQ(hit.entity, nearBox.getEntity());

    // RayReturn's fields
    const RayReturn asPhysics = hit.toRayReturn();
    EXPECT_TRUE(asPhysics.hit);
    EXPECT_EQ(asPhysics.body, nearBox.getEntity());
}
