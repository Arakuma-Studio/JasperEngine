// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Old 3D physics scenes simulate as they did (physics-3d T1): bodies, colliders and joints
// decoded from the scene format written before the Rigidbody fields, per-collider triggers and
// materials and joint parameters existed run 120 fixed steps to the poses recorded on the code
// before the feature, committed as fixtures/physics3d/legacy_run.csv. With
// JASPER_RECORD_PHYSICS3D_FIXTURE=1 the test writes that file instead of comparing. The cone
// joint is left out: its swing limit was 0 before (the documented fix, Physics3DJointsTest).

#include <gtest/gtest.h>

#include "Scene.h"
#include "Stream.h"
#include "component/Joint3DComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "object/physics/Rigidbody.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "yaml-cpp/yaml.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace Physics3DLegacyTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    const float physics3DLegacyStep = 1.0f / 60.0f;

    std::string physics3DLegacyFixturePath() {
        return std::string(JASPER_EDITOR_TEST_FIXTURES) + "/physics3d/legacy_run.csv";
    }

    Entity physics3DLegacyEntity(Scene& scene, const Vector3& position, const Quaternion& rotation = Quaternion::IDENTITY) {
        Entity entity = scene.createEntity();
        Transform transform;
        transform.position = position;
        transform.rotation = rotation;
        scene.addComponent<Transform>(entity, transform);
        return entity;
    }

    // a collider block as the editor wrote it before (no isTrigger, material, receiver)
    std::string physics3DLegacyCollider(const std::string& type, const std::string& position, float width, float height, float depth,
                               float radius, float halfHeight, float density, const std::string& source = "none",
                               const std::string& vertices = "[]", const std::string& indices = "[]") {
        std::ostringstream yaml;
        yaml << "  - type: " << type << "\n"
             << "    position: " << position << "\n"
             << "    rotation: [1, 0, 0, 0]\n"
             << "    width: " << width << "\n"
             << "    height: " << height << "\n"
             << "    depth: " << depth << "\n"
             << "    radius: " << radius << "\n"
             << "    halfHeight: " << halfHeight << "\n"
             << "    topRadius: 0.5\n"
             << "    bottomRadius: 0.5\n"
             << "    density: " << density << "\n"
             << "    source: " << source << "\n"
             << "    sourceEntity: 0\n"
             << "    samplesSize: 0\n"
             << "    vertices: " << vertices << "\n"
             << "    indices: " << indices << "\n";
        return yaml.str();
    }

    std::string physics3DLegacyBody(const std::string& type, const std::vector<std::string>& colliders, float friction = 0.2f,
                           float restitution = 0.0f, float gravityFactor = 1.0f, const std::string& motionQuality = "discrete") {
        std::ostringstream yaml;
        yaml << "enabled: true\n"
             << "type: " << type << "\n"
             << "motionQuality: " << motionQuality << "\n"
             << "sensor: false\n"
             << "gravityFactor: " << gravityFactor << "\n"
             << "friction: " << friction << "\n"
             << "restitution: " << restitution << "\n"
             << "buoyancy: 0\n"
             << "waterDrag: 0.5\n"
             << "waterAngularDrag: 0.01\n"
             << "allowedDOFs: {translationX: true, translationY: true, translationZ: true, rotationX: true, rotationY: true, rotationZ: true}\n"
             << "numColliders: " << colliders.size() << "\n"
             << "colliders:\n";
        for (const std::string& collider : colliders) yaml << collider;
        return yaml.str();
    }

    std::string physics3DLegacyJoint(const std::string& type, Entity bodyA, Entity bodyB, const std::string& extra) {
        std::ostringstream yaml;
        yaml << "enabled: true\n"
             << "type: " << type << "\n"
             << "bodyA: " << bodyA << "\n"
             << "bodyB: " << bodyB << "\n"
             << "anchorA: [0, 0, 0]\n"
             << "anchorB: [0, 0, 0]\n"
             << "anchor: [0, 0, 0]\n"
             << "axis: [0, 0, 0]\n"
             << "normal: [0, 0, 0]\n"
             << "twistAxis: [0, 0, 0]\n"
             << "planeAxis: [0, 0, 0]\n"
             << "axisX: [0, 0, 0]\n"
             << "axisY: [0, 0, 0]\n"
             << "limitsMin: 0\n"
             << "limitsMax: 0\n"
             << "normalHalfConeAngle: 45\n"
             << "planeHalfConeAngle: 45\n"
             << "twistMinAngle: -45\n"
             << "twistMaxAngle: 45\n"
             << "autoAnchors: true\n";
        // later keys win in yaml-cpp's map: the joint's own values
        YAML::Node node = YAML::Load(yaml.str());
        YAML::Node overrides = YAML::Load(extra);
        for (auto it = overrides.begin(); it != overrides.end(); ++it) {
            node[it->first.as<std::string>()] = it->second;
        }
        std::ostringstream out;
        out << node;
        return out.str();
    }

    void physics3DLegacyAddBody(Scene& scene, Entity entity, const std::string& yaml) {
        scene.addComponent<RigidbodyComponent>(entity, Stream::decodeRigidbodyComponent(YAML::Load(yaml)));
    }

    void physics3DLegacyAddJoint(Scene& scene, Entity entity, const std::string& yaml) {
        scene.addComponent<Joint3DComponent>(entity, Stream::decodeJoint3DComponent(YAML::Load(yaml)));
    }

    void physics3DLegacyResolveTransforms(Scene& scene) {
        auto transforms = scene.getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++) {
            Transform& transform = transforms->getComponentFromIndex(i);
            if (transform.needUpdate) {
                scene.getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
                transform.needUpdate = false;
            }
        }
    }

    Quaternion physics3DLegacyDegreesAbout(float degrees, const Vector3& axis) {
        return Quaternion(Angle::degToDefault(degrees), axis);
    }

    // The legacy scene: a static ground with a raw-mesh ramp, falling bodies of every authored
    // kind (box, sphere, a two-collider compound, a raw convex hull, a linear-cast capsule), a
    // hinge pendulum, a limited slider, a distance rope, a point-joint chain, a swing-twist arm,
    // an unconstrained six-DOF pair and a welded pair, all with world anchors. Returns, every 10
    // steps, the pose of each dynamic body: "step,body,x,y,z,qw,qx,qy,qz".
    std::vector<std::string> physics3DLegacyRunLegacyScene() {
        Scene scene;
        scene.setPhysicsInterpolation(false);

        const Entity ground = physics3DLegacyEntity(scene, Vector3(0, -0.5f, 0));
        physics3DLegacyAddBody(scene, ground, physics3DLegacyBody("static", {physics3DLegacyCollider("box", "[0, 0, 0]", 60, 1, 60, 0.5f, 0.5f, 1000)}));

        // a ramp: two triangles of a raw mesh, uint16 indices
        const Entity ramp = physics3DLegacyEntity(scene, Vector3(10, 0, 0));
        physics3DLegacyAddBody(scene, ramp, physics3DLegacyBody("static", {physics3DLegacyCollider("mesh", "[0, 0, 0]", 1, 1, 1, 0.5f, 0.5f, 1000, "raw_mesh",
            "[[-3, 0, -3], [3, 3, -3], [3, 3, 3], [-3, 0, 3]]", "[0, 1, 2, 0, 2, 3]")}, 0.4f));

        const Entity crate = physics3DLegacyEntity(scene, Vector3(0, 3, 0), physics3DLegacyDegreesAbout(20.0f, Vector3(0, 0, 1)));
        physics3DLegacyAddBody(scene, crate, physics3DLegacyBody("dynamic", {physics3DLegacyCollider("box", "[0, 0, 0]", 1, 1, 1, 0.5f, 0.5f, 1000)}, 0.2f, 0.3f));

        const Entity ball = physics3DLegacyEntity(scene, Vector3(2, 4, 1));
        physics3DLegacyAddBody(scene, ball, physics3DLegacyBody("dynamic", {physics3DLegacyCollider("sphere", "[0, 0, 0]", 1, 1, 1, 0.5f, 0.5f, 500)}, 0.5f));

        const Entity compound = physics3DLegacyEntity(scene, Vector3(-2, 3, 0), physics3DLegacyDegreesAbout(35.0f, Vector3(1, 0, 0)));
        physics3DLegacyAddBody(scene, compound, physics3DLegacyBody("dynamic", {
            physics3DLegacyCollider("box", "[0, 0, 0]", 1.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1000),
            physics3DLegacyCollider("sphere", "[0.75, 0.25, 0]", 1, 1, 1, 0.35f, 0.5f, 2000)}, 0.3f));

        const Entity hull = physics3DLegacyEntity(scene, Vector3(10, 6, 0), physics3DLegacyDegreesAbout(15.0f, Vector3(0, 1, 0)));
        physics3DLegacyAddBody(scene, hull, physics3DLegacyBody("dynamic", {physics3DLegacyCollider("convex_hull", "[0, 0, 0]", 1, 1, 1, 0.5f, 0.5f, 800, "raw_vertices",
            "[[-0.5, -0.5, -0.5], [0.5, -0.5, -0.5], [0, -0.5, 0.6], [0, 0.6, 0]]")}, 0.6f));

        const Entity capsule = physics3DLegacyEntity(scene, Vector3(4, 2, -3), physics3DLegacyDegreesAbout(80.0f, Vector3(0, 0, 1)));
        physics3DLegacyAddBody(scene, capsule, physics3DLegacyBody("dynamic", {physics3DLegacyCollider("capsule", "[0, 0, 0]", 1, 1, 1, 0.3f, 0.6f, 1000)},
            0.2f, 0.0f, 0.8f, "linear_cast"));

        // a hinge pendulum about z at a world point
        const Entity pivot = physics3DLegacyEntity(scene, Vector3(-6, 6, 0));
        physics3DLegacyAddBody(scene, pivot, physics3DLegacyBody("static", {physics3DLegacyCollider("sphere", "[0, 0, 0]", 1, 1, 1, 0.1f, 0.5f, 1000)}));
        const Entity bob = physics3DLegacyEntity(scene, Vector3(-4.5f, 6, 0));
        physics3DLegacyAddBody(scene, bob, physics3DLegacyBody("dynamic", {physics3DLegacyCollider("box", "[0, 0, 0]", 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1000)}));
        physics3DLegacyAddJoint(scene, physics3DLegacyEntity(scene, Vector3(0, 0, 0)), physics3DLegacyJoint("hinge", pivot, bob,
            "{anchor: [-6, 6, 0], axis: [0, 0, 1], normal: [1, 0, 0]}"));

        // a slider along y with limits containing 0
        const Entity rail = physics3DLegacyEntity(scene, Vector3(-10, 6, 0));
        physics3DLegacyAddBody(scene, rail, physics3DLegacyBody("static", {physics3DLegacyCollider("box", "[0, 0, 0]", 0.2f, 0.2f, 0.2f, 0.5f, 0.5f, 1000)}));
        const Entity carriage = physics3DLegacyEntity(scene, Vector3(-10, 6, 1));
        physics3DLegacyAddBody(scene, carriage, physics3DLegacyBody("dynamic", {physics3DLegacyCollider("box", "[0, 0, 0]", 0.4f, 0.4f, 0.4f, 0.5f, 0.5f, 1000)}));
        physics3DLegacyAddJoint(scene, physics3DLegacyEntity(scene, Vector3(0, 0, 0)), physics3DLegacyJoint("prismatic", rail, carriage,
            "{axis: [0, 1, 0], limitsMin: -1.5, limitsMax: 2}"));

        // a rope from a hook with world anchors
        const Entity hook = physics3DLegacyEntity(scene, Vector3(6, 8, 4));
        physics3DLegacyAddBody(scene, hook, physics3DLegacyBody("static", {physics3DLegacyCollider("sphere", "[0, 0, 0]", 1, 1, 1, 0.1f, 0.5f, 1000)}));
        const Entity weight = physics3DLegacyEntity(scene, Vector3(7.5f, 7, 4));
        physics3DLegacyAddBody(scene, weight, physics3DLegacyBody("dynamic", {physics3DLegacyCollider("sphere", "[0, 0, 0]", 1, 1, 1, 0.25f, 0.5f, 1000)}, 0.2f, 0.2f));
        physics3DLegacyAddJoint(scene, physics3DLegacyEntity(scene, Vector3(0, 0, 0)), physics3DLegacyJoint("distance", hook, weight,
            "{anchorA: [6, 8, 4], anchorB: [7.5, 7, 4], autoAnchors: false}"));

        // a chain of two links on point joints
        const Entity anchorBody = physics3DLegacyEntity(scene, Vector3(0, 9, -6));
        physics3DLegacyAddBody(scene, anchorBody, physics3DLegacyBody("static", {physics3DLegacyCollider("sphere", "[0, 0, 0]", 1, 1, 1, 0.1f, 0.5f, 1000)}));
        const Entity link1 = physics3DLegacyEntity(scene, Vector3(0.6f, 9, -6));
        physics3DLegacyAddBody(scene, link1, physics3DLegacyBody("dynamic", {physics3DLegacyCollider("box", "[0, 0, 0]", 1, 0.2f, 0.2f, 0.5f, 0.5f, 1000)}));
        const Entity link2 = physics3DLegacyEntity(scene, Vector3(1.8f, 9, -6));
        physics3DLegacyAddBody(scene, link2, physics3DLegacyBody("dynamic", {physics3DLegacyCollider("box", "[0, 0, 0]", 1, 0.2f, 0.2f, 0.5f, 0.5f, 1000)}));
        physics3DLegacyAddJoint(scene, physics3DLegacyEntity(scene, Vector3(0, 0, 0)), physics3DLegacyJoint("point", anchorBody, link1, "{anchor: [0, 9, -6]}"));
        physics3DLegacyAddJoint(scene, physics3DLegacyEntity(scene, Vector3(0, 0, 0)), physics3DLegacyJoint("point", link1, link2, "{anchor: [1.2, 9, -6]}"));

        // a swing-twist arm with its default limits
        const Entity shoulder = physics3DLegacyEntity(scene, Vector3(-6, 8, -6));
        physics3DLegacyAddBody(scene, shoulder, physics3DLegacyBody("static", {physics3DLegacyCollider("sphere", "[0, 0, 0]", 1, 1, 1, 0.1f, 0.5f, 1000)}));
        const Entity arm = physics3DLegacyEntity(scene, Vector3(-5, 8, -6), physics3DLegacyDegreesAbout(90.0f, Vector3(0, 0, 1)));
        physics3DLegacyAddBody(scene, arm, physics3DLegacyBody("dynamic", {physics3DLegacyCollider("capsule", "[0, 0, 0]", 1, 1, 1, 0.15f, 0.7f, 1000)}));
        physics3DLegacyAddJoint(scene, physics3DLegacyEntity(scene, Vector3(0, 0, 0)), physics3DLegacyJoint("swingtwist", shoulder, arm,
            "{anchor: [-6, 8, -6], twistAxis: [1, 0, 0], planeAxis: [0, 1, 0], normalHalfConeAngle: 30, planeHalfConeAngle: 20, twistMinAngle: -10, twistMaxAngle: 10}"));

        // a six-DOF pair with every axis free (Jolt's defaults), so it falls freely
        const Entity freeA = physics3DLegacyEntity(scene, Vector3(6, 3, -6));
        physics3DLegacyAddBody(scene, freeA, physics3DLegacyBody("dynamic", {physics3DLegacyCollider("box", "[0, 0, 0]", 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1000)}));
        const Entity freeB = physics3DLegacyEntity(scene, Vector3(6, 4.5f, -6));
        physics3DLegacyAddBody(scene, freeB, physics3DLegacyBody("dynamic", {physics3DLegacyCollider("box", "[0, 0, 0]", 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1000)}));
        physics3DLegacyAddJoint(scene, physics3DLegacyEntity(scene, Vector3(0, 0, 0)), physics3DLegacyJoint("sixdof", freeA, freeB,
            "{anchorA: [6, 3.5, -6], anchorB: [6, 4, -6], axisX: [1, 0, 0], axisY: [0, 1, 0]}"));

        // a welded pair: an L dropped on its corner
        const Entity weldA = physics3DLegacyEntity(scene, Vector3(-3, 5, 5));
        physics3DLegacyAddBody(scene, weldA, physics3DLegacyBody("dynamic", {physics3DLegacyCollider("box", "[0, 0, 0]", 1, 0.3f, 0.3f, 0.5f, 0.5f, 1000)}));
        const Entity weldB = physics3DLegacyEntity(scene, Vector3(-2.35f, 5.65f, 5));
        physics3DLegacyAddBody(scene, weldB, physics3DLegacyBody("dynamic", {physics3DLegacyCollider("box", "[0, 0, 0]", 0.3f, 1, 0.3f, 0.5f, 0.5f, 1000)}));
        physics3DLegacyAddJoint(scene, physics3DLegacyEntity(scene, Vector3(0, 0, 0)), physics3DLegacyJoint("fixed", weldA, weldB, "{}"));

        physics3DLegacyResolveTransforms(scene);
        scene.getSystem<PhysicsSystem>()->load();

        const std::vector<std::pair<std::string, Entity>> dynamics = {
            {"crate", crate}, {"ball", ball}, {"compound", compound}, {"hull", hull}, {"capsule", capsule},
            {"bob", bob}, {"carriage", carriage}, {"weight", weight}, {"link1", link1}, {"link2", link2},
            {"arm", arm}, {"freeA", freeA}, {"freeB", freeB}, {"weldA", weldA}, {"weldB", weldB}};

        std::vector<std::string> rows;
        for (int stepIndex = 1; stepIndex <= 120; stepIndex++) {
            scene.getSystem<PhysicsSystem>()->fixedUpdate(physics3DLegacyStep);
            physics3DLegacyResolveTransforms(scene);
            if (stepIndex % 10 != 0) continue;
            for (const auto& [name, entity] : dynamics) {
                Rigidbody body(&scene, entity);
                const Vector3 position = body.getPosition();
                Quaternion rotation = body.getRotation();
                // q and -q are the same rotation: w >= 0 so both runs write the same one
                if (rotation.w < 0.0f) rotation = Quaternion(-rotation.w, -rotation.x, -rotation.y, -rotation.z);
                char row[256];
                std::snprintf(row, sizeof(row), "%d,%s,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f", stepIndex, name.c_str(),
                              position.x, position.y, position.z, rotation.w, rotation.x, rotation.y, rotation.z);
                rows.push_back(row);
            }
        }
        return rows;
    }

}

TEST(Physics3DLegacy, OldSceneSimulatesAsRecordedBeforeTheFeature) {
    const std::vector<std::string> rows = physics3DLegacyRunLegacyScene();

    if (const char* record = std::getenv("JASPER_RECORD_PHYSICS3D_FIXTURE"); record && std::string(record) == "1") {
        std::ofstream out(physics3DLegacyFixturePath());
        ASSERT_TRUE(out.good()) << physics3DLegacyFixturePath();
        out << "step,body,x,y,z,qw,qx,qy,qz\n";
        for (const std::string& row : rows) out << row << "\n";
        GTEST_SKIP() << "recorded " << physics3DLegacyFixturePath();
    }

    std::ifstream in(physics3DLegacyFixturePath());
    ASSERT_TRUE(in.good()) << "missing fixture " << physics3DLegacyFixturePath();
    std::string line;
    std::getline(in, line); // header
    size_t index = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        ASSERT_LT(index, rows.size());
        int stepA = 0, stepB = 0;
        char nameA[32] = {0}, nameB[32] = {0};
        float a[7], b[7];
        ASSERT_EQ(std::sscanf(line.c_str(), "%d,%31[^,],%f,%f,%f,%f,%f,%f,%f", &stepA, nameA, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6]), 9) << line;
        ASSERT_EQ(std::sscanf(rows[index].c_str(), "%d,%31[^,],%f,%f,%f,%f,%f,%f,%f", &stepB, nameB, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6]), 9) << rows[index];
        EXPECT_EQ(stepA, stepB);
        EXPECT_STREQ(nameA, nameB);
        for (int k = 0; k < 3; k++) {
            EXPECT_NEAR(a[k], b[k], 1e-5f) << line << " vs " << rows[index];
        }
        for (int k = 3; k < 7; k++) {
            EXPECT_NEAR(a[k], b[k], 1e-4f) << line << " vs " << rows[index];
        }
        index++;
    }
    EXPECT_EQ(index, rows.size());
}

}  // namespace Physics3DLegacyTest_cpp
