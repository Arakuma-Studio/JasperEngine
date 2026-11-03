// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Saving and exporting the physics-3d fields (spec T17, T15's exported CMake): every new Rigidbody,
// collider, Joint 3D and Ragdoll field survives encode / decode, files of before read with the
// legacy values, and the exported-build code sets every non-default field with no collider caps.

#include <gtest/gtest.h>

#include "Physics3DTestWorld.h"

#include "Factory.h"
#include "Stream.h"
#include "component/RagdollComponent.h"
#include "yaml-cpp/yaml.h"

#include <cmath>
#include <limits>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::physics3dtest;

TEST(Physics3DSerialization, EveryBodyAndColliderFieldRoundTrips) {
    RigidbodyComponent body;
    body.type = BodyType::DYNAMIC;
    body.material = "physics/body.physicsmaterial";
    body.linearDamping = 0.5f;
    body.angularDamping = 0.7f;
    body.maxLinearVelocity = 40.0f;
    body.maxAngularVelocity = 9.0f;
    body.automaticMass = false;
    body.mass = 3.5f;
    body.automaticCenterOfMass = false;
    body.centerOfMass = Vector3(0, -0.5f, 0.25f);
    body.automaticInertiaTensor = false;
    body.inertiaTensor = Vector3(1, 100, 2);
    body.inertiaTensorRotation = degreesAbout(30.0f, Vector3(0, 1, 0));
    body.allowSleeping = false;
    body.collideKinematicVsNonDynamic = true;
    body.interpolation = RigidbodyInterpolation::EXTRAPOLATE;
    Collider3D box;
    box.type = Collider3DType::BOX;
    box.isTrigger = true;
    box.material = "physics/ice.physicsmaterial";
    box.receiver = 12;
    body.colliders.push_back(box);
    Collider3D mesh;
    mesh.type = Collider3DType::MESH;
    mesh.source = Collider3DSource::RAW_MESH;
    for (int i = 0; i < 100; i++) mesh.vertices.push_back(Vector3((float)i, 0, (float)(i % 7)));
    for (uint32_t i = 0; i + 2 < 100; i++) mesh.indices.insert(mesh.indices.end(), {i, i + 1, i + 2});
    body.colliders.push_back(mesh);

    const YAML::Node node = Stream::encodeRigidbodyComponent(body);
    EXPECT_EQ(node["numColliders"].as<unsigned int>(), 2u) << "still written for older editors";
    const RigidbodyComponent decoded = Stream::decodeRigidbodyComponent(YAML::Load(YAML::Dump(node)));
    EXPECT_EQ(YAML::Dump(Stream::encodeRigidbodyComponent(decoded)), YAML::Dump(node));
    EXPECT_EQ(decoded.material, body.material);
    EXPECT_FLOAT_EQ(decoded.linearDamping, 0.5f);
    EXPECT_FLOAT_EQ(decoded.maxAngularVelocity, 9.0f);
    EXPECT_FALSE(decoded.automaticMass);
    EXPECT_FLOAT_EQ(decoded.mass, 3.5f);
    EXPECT_EQ(decoded.centerOfMass, body.centerOfMass);
    EXPECT_EQ(decoded.inertiaTensor, body.inertiaTensor);
    EXPECT_FALSE(decoded.allowSleeping);
    EXPECT_TRUE(decoded.collideKinematicVsNonDynamic);
    EXPECT_EQ(decoded.interpolation, RigidbodyInterpolation::EXTRAPOLATE);
    ASSERT_EQ(decoded.colliders.size(), 2u);
    EXPECT_TRUE(decoded.colliders[0].isTrigger);
    EXPECT_EQ(decoded.colliders[0].material, box.material);
    EXPECT_EQ(decoded.colliders[0].receiver, 12u);
    EXPECT_EQ(decoded.colliders[1].vertices.size(), 100u);
    EXPECT_EQ(decoded.colliders[1].indices.size(), mesh.indices.size());

    // the exported-build code: every non-default field, no caps
    Scene scene;
    const Entity e = scene.createEntity();
    scene.addComponent<RigidbodyComponent>(e, body);
    const std::string code = Factory::createRigidbodyComponent(4, &scene, e, "scene", "entity", false, nullptr);
    for (const char* expected : {"rigidbody.material = \"physics/body.physicsmaterial\";", "rigidbody.linearDamping = 0.500000f;",
                                 "rigidbody.automaticMass = false;", "rigidbody.mass = 3.500000f;", "rigidbody.automaticCenterOfMass = false;",
                                 "rigidbody.inertiaTensor = Vector3(1.000000f, 100.000000f, 2.000000f);", "rigidbody.allowSleeping = false;",
                                 "rigidbody.collideKinematicVsNonDynamic = true;", "rigidbody.interpolation = RigidbodyInterpolation::EXTRAPOLATE;",
                                 "rigidbody.colliders.resize(2);", "rigidbody.colliders[0].isTrigger = true;",
                                 "rigidbody.colliders[0].material = \"physics/ice.physicsmaterial\";", "rigidbody.colliders[1].vertices = {",
                                 "rigidbody.colliders[1].indices = {"}) {
        EXPECT_NE(code.find(expected), std::string::npos) << expected;
    }
    EXPECT_EQ(code.find("numColliders"), std::string::npos);
    EXPECT_EQ(code.find("MAX_COLLIDER"), std::string::npos);
}

TEST(Physics3DSerialization, OldBodyFilesReadWithTheOldValues) {
    const RigidbodyComponent decoded = Stream::decodeRigidbodyComponent(YAML::Load(
        "type: dynamic\nfriction: 0.4\nrestitution: 0.1\nnumColliders: 1\n"
        "colliders:\n  - type: box\n    width: 2\n  - type: sphere\n    radius: 3\n"));
    EXPECT_FLOAT_EQ(decoded.linearDamping, 0.05f) << "Jolt's implicit damping";
    EXPECT_FLOAT_EQ(decoded.angularDamping, 0.05f);
    EXPECT_TRUE(decoded.automaticMass);
    EXPECT_TRUE(decoded.allowSleeping);
    EXPECT_EQ(decoded.interpolation, RigidbodyInterpolation::SCENE);
    ASSERT_EQ(decoded.colliders.size(), 1u) << "numColliders still bounds the list";
    EXPECT_FALSE(decoded.colliders[0].isTrigger);
    EXPECT_TRUE(decoded.colliders[0].material.empty());
}

TEST(Physics3DSerialization, EveryJointFieldRoundTripsAndOldJointsAreWorld) {
    Joint3DComponent joint = makeJoint3D(Joint3DType::SIXDOF);
    joint.bodyA = 3;
    joint.bodyB = 4;
    joint.anchor = Vector3(0.1f, 0.2f, 0.3f);
    joint.anchorB = Vector3(1, 2, 3);
    joint.enableCollision = true;
    joint.breakForce = 500.0f;
    joint.breakTorque = 80.0f;
    joint.breakAction = JointBreakAction::CALLBACK_ONLY;
    joint.useLimits = true;
    joint.limitsSpringFrequency = 4.0f;
    joint.limitsSpringDamping = 0.5f;
    joint.useMotor = true;
    joint.motorTargetVelocity = 90.0f;
    joint.motorMaxForce = 250.0f;
    joint.useSpring = true;
    joint.springStiffness = 50.0f;
    joint.springDamper = 5.0f;
    joint.springTargetPosition = 10.0f;
    joint.maxFriction = 2.0f;
    joint.minDistance = 1.0f;
    joint.maxDistance = 2.0f;
    joint.coneHalfAngle = 30.0f;
    joint.swingType = JointSwingType::PYRAMID;
    joint.motorTargetRotation = degreesAbout(20.0f, Vector3(1, 0, 0));
    joint.motorSpringFrequency = 8.0f;
    joint.motorSpringDamping = 0.7f;
    joint.xMotion = JointMotion::LOCKED;
    joint.yMotion = JointMotion::LIMITED;
    joint.angularZMotion = JointMotion::LOCKED;
    joint.linearLimitMin = Vector3(0, -1, 0);
    joint.linearLimitMax = Vector3(0, 1, 0);
    joint.angularLimitMin = Vector3(-10, 0, 0);
    joint.angularLimitMax = Vector3(10, 0, 0);
    joint.useDrive = true;
    joint.driveStiffness = 100.0f;
    joint.driveDamping = 10.0f;
    joint.angularDriveStiffness = 20.0f;
    joint.angularDriveDamping = 2.0f;
    joint.driveMaxForce = 1000.0f;
    joint.targetPosition = Vector3(0, 0.5f, 0);
    joint.targetRotation = degreesAbout(15.0f, Vector3(0, 0, 1));
    joint.targetVelocity = Vector3(1, 0, 0);
    joint.targetAngularVelocity = Vector3(0, 30, 0);

    const YAML::Node node = Stream::encodeJoint3DComponent(joint);
    const Joint3DComponent decoded = Stream::decodeJoint3DComponent(YAML::Load(YAML::Dump(node)));
    EXPECT_EQ(YAML::Dump(Stream::encodeJoint3DComponent(decoded)), YAML::Dump(node));
    EXPECT_EQ(decoded.anchorSpace, JointAnchorSpace::LOCAL);
    EXPECT_FLOAT_EQ(decoded.breakForce, 500.0f);
    EXPECT_EQ(decoded.breakAction, JointBreakAction::CALLBACK_ONLY);
    EXPECT_EQ(decoded.yMotion, JointMotion::LIMITED);
    EXPECT_FLOAT_EQ(decoded.driveMaxForce, 1000.0f);
    EXPECT_TRUE(std::isinf(decoded.driveMaxTorque)) << "an infinite value is not written and reads back infinite";

    // a joint written before the fields: world anchors, the bodies collide, no limits but the slider's
    const Joint3DComponent hinge = Stream::decodeJoint3DComponent(YAML::Load("type: hinge\nbodyA: 1\nbodyB: 2\nanchor: [0, 1, 0]\naxis: [0, 0, 1]\n"));
    EXPECT_EQ(hinge.anchorSpace, JointAnchorSpace::WORLD);
    EXPECT_TRUE(hinge.enableCollision);
    EXPECT_FALSE(hinge.useLimits);
    EXPECT_TRUE(std::isinf(hinge.breakForce));
    const Joint3DComponent slider = Stream::decodeJoint3DComponent(YAML::Load("type: prismatic\nbodyA: 1\nbodyB: 2\nlimitsMin: -1\nlimitsMax: 1\n"));
    EXPECT_TRUE(slider.useLimits);

    Scene scene;
    const Entity e = scene.createEntity();
    scene.addComponent<Joint3DComponent>(e, joint);
    const std::string code = Factory::createComponent(4, &scene, e, ComponentType::Joint3DComponent, std::filesystem::path(), "scene", "entity");
    for (const char* expected : {"joint3d.anchorSpace = JointAnchorSpace::LOCAL;", "joint3d.enableCollision = true;",
                                 "joint3d.breakForce = 500.000000f;", "joint3d.breakAction = JointBreakAction::CALLBACK_ONLY;",
                                 "joint3d.useMotor = true;", "joint3d.swingType = JointSwingType::PYRAMID;",
                                 "joint3d.yMotion = JointMotion::LIMITED;", "joint3d.useDrive = true;", "joint3d.coneHalfAngle = "}) {
        EXPECT_NE(code.find(expected), std::string::npos) << expected;
    }
    EXPECT_EQ(code.find("inf"), std::string::npos) << "infinite values are left at the default";
}

TEST(Physics3DSerialization, RagdollRoundTrips) {
    RagdollComponent ragdoll;
    ragdoll.simulated = true;
    ragdoll.root = 5;
    ragdoll.animator = 6;
    ragdoll.inheritVelocity = false;
    ragdoll.selfCollision = false;
    ragdoll.totalMass = 70.0f;
    ragdoll.enabled = false;
    const YAML::Node node = Stream::encodeRagdollComponent(ragdoll);
    const RagdollComponent decoded = Stream::decodeRagdollComponent(YAML::Load(YAML::Dump(node)));
    EXPECT_EQ(YAML::Dump(Stream::encodeRagdollComponent(decoded)), YAML::Dump(node));
    EXPECT_TRUE(decoded.simulated);
    EXPECT_EQ(decoded.root, 5u);
    EXPECT_EQ(decoded.animator, 6u);
    EXPECT_FALSE(decoded.inheritVelocity);
    EXPECT_FALSE(decoded.selfCollision);
    EXPECT_FLOAT_EQ(decoded.totalMass, 70.0f);
    EXPECT_FALSE(decoded.enabled);

    Scene scene;
    const Entity e = scene.createEntity();
    scene.addComponent<RagdollComponent>(e, ragdoll);
    const std::string code = Factory::createComponent(4, &scene, e, ComponentType::RagdollComponent, std::filesystem::path(), "scene", "entity");
    EXPECT_NE(code.find("ragdoll.simulated = true;"), std::string::npos);
    EXPECT_NE(code.find("ragdoll.totalMass = 70.000000f;"), std::string::npos);
}
