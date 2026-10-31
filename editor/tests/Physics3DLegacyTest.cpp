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

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    const float legacyStep = 1.0f / 60.0f;

    std::string fixturePath() {
        return std::string(JASPER_EDITOR_TEST_FIXTURES) + "/physics3d/legacy_run.csv";
    }

    Entity legacyEntity(Scene& scene, const Vector3& position, const Quaternion& rotation = Quaternion::IDENTITY) {
        Entity entity = scene.createEntity();
        Transform transform;
        transform.position = position;
        transform.rotation = rotation;
        scene.addComponent<Transform>(entity, transform);
        return entity;
    }

    // a collider block as the editor wrote it before (no isTrigger, material, receiver)
    std::string legacyCollider(const std::string& type, const std::string& position, float width, float height, float depth,
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

    std::string legacyBody(const std::string& type, const std::vector<std::string>& colliders, float friction = 0.2f,
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

    std::string legacyJoint(const std::string& type, Entity bodyA, Entity bodyB, const std::string& extra) {
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

    void addBody(Scene& scene, Entity entity, const std::string& yaml) {
        scene.addComponent<RigidbodyComponent>(entity, Stream::decodeRigidbodyComponent(YAML::Load(yaml)));
    }

    void addJoint(Scene& scene, Entity entity, const std::string& yaml) {
        scene.addComponent<Joint3DComponent>(entity, Stream::decodeJoint3DComponent(YAML::Load(yaml)));
    }

    void resolveTransforms(Scene& scene) {
        auto transforms = scene.getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++) {
            Transform& transform = transforms->getComponentFromIndex(i);
            if (transform.needUpdate) {
                scene.getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
                transform.needUpdate = false;
            }
        }
    }

    Quaternion degreesAbout(float degrees, const Vector3& axis) {
        return Quaternion(Angle::degToDefault(degrees), axis);
    }

    // The legacy scene: a static ground with a raw-mesh ramp, falling bodies of every authored
    // kind (box, sphere, a two-collider compound, a raw convex hull, a linear-cast capsule), a
    // hinge pendulum, a limited slider, a distance rope, a point-joint chain, a swing-twist arm,
    // an unconstrained six-DOF pair and a welded pair, all with world anchors. Returns, every 10
    // steps, the pose of each dynamic body: "step,body,x,y,z,qw,qx,qy,qz".
    std::vector<std::string> runLegacyScene() {
        Scene scene;
        scene.setPhysicsInterpolation(false);

        const Entity ground = legacyEntity(scene, Vector3(0, -0.5f, 0));
        addBody(scene, ground, legacyBody("static", {legacyCollider("box", "[0, 0, 0]", 60, 1, 60, 0.5f, 0.5f, 1000)}));

        // a ramp: two triangles of a raw mesh, uint16 indices
        const Entity ramp = legacyEntity(scene, Vector3(10, 0, 0));
        addBody(scene, ramp, legacyBody("static", {legacyCollider("mesh", "[0, 0, 0]", 1, 1, 1, 0.5f, 0.5f, 1000, "raw_mesh",
            "[[-3, 0, -3], [3, 3, -3], [3, 3, 3], [-3, 0, 3]]", "[0, 1, 2, 0, 2, 3]")}, 0.4f));

        const Entity crate = legacyEntity(scene, Vector3(0, 3, 0), degreesAbout(20.0f, Vector3(0, 0, 1)));
        addBody(scene, crate, legacyBody("dynamic", {legacyCollider("box", "[0, 0, 0]", 1, 1, 1, 0.5f, 0.5f, 1000)}, 0.2f, 0.3f));

        const Entity ball = legacyEntity(scene, Vector3(2, 4, 1));
        addBody(scene, ball, legacyBody("dynamic", {legacyCollider("sphere", "[0, 0, 0]", 1, 1, 1, 0.5f, 0.5f, 500)}, 0.5f));

        const Entity compound = legacyEntity(scene, Vector3(-2, 3, 0), degreesAbout(35.0f, Vector3(1, 0, 0)));
        addBody(scene, compound, legacyBody("dynamic", {
            legacyCollider("box", "[0, 0, 0]", 1.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1000),
            legacyCollider("sphere", "[0.75, 0.25, 0]", 1, 1, 1, 0.35f, 0.5f, 2000)}, 0.3f));

        const Entity hull = legacyEntity(scene, Vector3(10, 6, 0), degreesAbout(15.0f, Vector3(0, 1, 0)));
        addBody(scene, hull, legacyBody("dynamic", {legacyCollider("convex_hull", "[0, 0, 0]", 1, 1, 1, 0.5f, 0.5f, 800, "raw_vertices",
            "[[-0.5, -0.5, -0.5], [0.5, -0.5, -0.5], [0, -0.5, 0.6], [0, 0.6, 0]]")}, 0.6f));

        const Entity capsule = legacyEntity(scene, Vector3(4, 2, -3), degreesAbout(80.0f, Vector3(0, 0, 1)));
        addBody(scene, capsule, legacyBody("dynamic", {legacyCollider("capsule", "[0, 0, 0]", 1, 1, 1, 0.3f, 0.6f, 1000)},
            0.2f, 0.0f, 0.8f, "linear_cast"));

        // a hinge pendulum about z at a world point
        const Entity pivot = legacyEntity(scene, Vector3(-6, 6, 0));
        addBody(scene, pivot, legacyBody("static", {legacyCollider("sphere", "[0, 0, 0]", 1, 1, 1, 0.1f, 0.5f, 1000)}));
        const Entity bob = legacyEntity(scene, Vector3(-4.5f, 6, 0));
        addBody(scene, bob, legacyBody("dynamic", {legacyCollider("box", "[0, 0, 0]", 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1000)}));
        addJoint(scene, legacyEntity(scene, Vector3(0, 0, 0)), legacyJoint("hinge", pivot, bob,
            "{anchor: [-6, 6, 0], axis: [0, 0, 1], normal: [1, 0, 0]}"));

        // a slider along y with limits containing 0
        const Entity rail = legacyEntity(scene, Vector3(-10, 6, 0));
        addBody(scene, rail, legacyBody("static", {legacyCollider("box", "[0, 0, 0]", 0.2f, 0.2f, 0.2f, 0.5f, 0.5f, 1000)}));
        const Entity carriage = legacyEntity(scene, Vector3(-10, 6, 1));
        addBody(scene, carriage, legacyBody("dynamic", {legacyCollider("box", "[0, 0, 0]", 0.4f, 0.4f, 0.4f, 0.5f, 0.5f, 1000)}));
        addJoint(scene, legacyEntity(scene, Vector3(0, 0, 0)), legacyJoint("prismatic", rail, carriage,
            "{axis: [0, 1, 0], limitsMin: -1.5, limitsMax: 2}"));

        // a rope from a hook with world anchors
        const Entity hook = legacyEntity(scene, Vector3(6, 8, 4));
        addBody(scene, hook, legacyBody("static", {legacyCollider("sphere", "[0, 0, 0]", 1, 1, 1, 0.1f, 0.5f, 1000)}));
        const Entity weight = legacyEntity(scene, Vector3(7.5f, 7, 4));
        addBody(scene, weight, legacyBody("dynamic", {legacyCollider("sphere", "[0, 0, 0]", 1, 1, 1, 0.25f, 0.5f, 1000)}, 0.2f, 0.2f));
        addJoint(scene, legacyEntity(scene, Vector3(0, 0, 0)), legacyJoint("distance", hook, weight,
            "{anchorA: [6, 8, 4], anchorB: [7.5, 7, 4], autoAnchors: false}"));

        // a chain of two links on point joints
        const Entity anchorBody = legacyEntity(scene, Vector3(0, 9, -6));
        addBody(scene, anchorBody, legacyBody("static", {legacyCollider("sphere", "[0, 0, 0]", 1, 1, 1, 0.1f, 0.5f, 1000)}));
        const Entity link1 = legacyEntity(scene, Vector3(0.6f, 9, -6));
        addBody(scene, link1, legacyBody("dynamic", {legacyCollider("box", "[0, 0, 0]", 1, 0.2f, 0.2f, 0.5f, 0.5f, 1000)}));
        const Entity link2 = legacyEntity(scene, Vector3(1.8f, 9, -6));
        addBody(scene, link2, legacyBody("dynamic", {legacyCollider("box", "[0, 0, 0]", 1, 0.2f, 0.2f, 0.5f, 0.5f, 1000)}));
        addJoint(scene, legacyEntity(scene, Vector3(0, 0, 0)), legacyJoint("point", anchorBody, link1, "{anchor: [0, 9, -6]}"));
        addJoint(scene, legacyEntity(scene, Vector3(0, 0, 0)), legacyJoint("point", link1, link2, "{anchor: [1.2, 9, -6]}"));

        // a swing-twist arm with its default limits
        const Entity shoulder = legacyEntity(scene, Vector3(-6, 8, -6));
        addBody(scene, shoulder, legacyBody("static", {legacyCollider("sphere", "[0, 0, 0]", 1, 1, 1, 0.1f, 0.5f, 1000)}));
        const Entity arm = legacyEntity(scene, Vector3(-5, 8, -6), degreesAbout(90.0f, Vector3(0, 0, 1)));
        addBody(scene, arm, legacyBody("dynamic", {legacyCollider("capsule", "[0, 0, 0]", 1, 1, 1, 0.15f, 0.7f, 1000)}));
        addJoint(scene, legacyEntity(scene, Vector3(0, 0, 0)), legacyJoint("swingtwist", shoulder, arm,
            "{anchor: [-6, 8, -6], twistAxis: [1, 0, 0], planeAxis: [0, 1, 0], normalHalfConeAngle: 30, planeHalfConeAngle: 20, twistMinAngle: -10, twistMaxAngle: 10}"));

        // a six-DOF pair with every axis free (Jolt's defaults), so it falls freely
        const Entity freeA = legacyEntity(scene, Vector3(6, 3, -6));
        addBody(scene, freeA, legacyBody("dynamic", {legacyCollider("box", "[0, 0, 0]", 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1000)}));
        const Entity freeB = legacyEntity(scene, Vector3(6, 4.5f, -6));
        addBody(scene, freeB, legacyBody("dynamic", {legacyCollider("box", "[0, 0, 0]", 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1000)}));
        addJoint(scene, legacyEntity(scene, Vector3(0, 0, 0)), legacyJoint("sixdof", freeA, freeB,
            "{anchorA: [6, 3.5, -6], anchorB: [6, 4, -6], axisX: [1, 0, 0], axisY: [0, 1, 0]}"));

        // a welded pair: an L dropped on its corner
        const Entity weldA = legacyEntity(scene, Vector3(-3, 5, 5));
        addBody(scene, weldA, legacyBody("dynamic", {legacyCollider("box", "[0, 0, 0]", 1, 0.3f, 0.3f, 0.5f, 0.5f, 1000)}));
        const Entity weldB = legacyEntity(scene, Vector3(-2.35f, 5.65f, 5));
        addBody(scene, weldB, legacyBody("dynamic", {legacyCollider("box", "[0, 0, 0]", 0.3f, 1, 0.3f, 0.5f, 0.5f, 1000)}));
        addJoint(scene, legacyEntity(scene, Vector3(0, 0, 0)), legacyJoint("fixed", weldA, weldB, "{}"));

        resolveTransforms(scene);
        scene.getSystem<PhysicsSystem>()->load();

        const std::vector<std::pair<std::string, Entity>> dynamics = {
            {"crate", crate}, {"ball", ball}, {"compound", compound}, {"hull", hull}, {"capsule", capsule},
            {"bob", bob}, {"carriage", carriage}, {"weight", weight}, {"link1", link1}, {"link2", link2},
            {"arm", arm}, {"freeA", freeA}, {"freeB", freeB}, {"weldA", weldA}, {"weldB", weldB}};

        std::vector<std::string> rows;
        for (int stepIndex = 1; stepIndex <= 120; stepIndex++) {
            scene.getSystem<PhysicsSystem>()->fixedUpdate(legacyStep);
            resolveTransforms(scene);
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
    const std::vector<std::string> rows = runLegacyScene();

    if (const char* record = std::getenv("JASPER_RECORD_PHYSICS3D_FIXTURE"); record && std::string(record) == "1") {
        std::ofstream out(fixturePath());
        ASSERT_TRUE(out.good()) << fixturePath();
        out << "step,body,x,y,z,qw,qx,qy,qz\n";
        for (const std::string& row : rows) out << row << "\n";
        GTEST_SKIP() << "recorded " << fixturePath();
    }

    std::ifstream in(fixturePath());
    ASSERT_TRUE(in.good()) << "missing fixture " << fixturePath();
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
