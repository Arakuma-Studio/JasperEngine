// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Old 2D physics scenes simulate as they did (physics-2d T2): bodies, colliders and joints
// decoded from the scene format written before 2D joints had their parameters (no anchorSpace,
// limits, motors, body fields...) run 120 fixed steps to the poses recorded on main before the
// feature, committed as fixtures/physics2d/legacy_run.csv. With JASPER_RECORD_PHYSICS2D_FIXTURE=1
// the test writes that file instead of comparing.

#include <gtest/gtest.h>

#include "Scene.h"
#include "Stream.h"
#include "component/Joint2DComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/Transform.h"
#include "object/physics/Rigidbody2D.h"
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
        return std::string(JASPER_EDITOR_TEST_FIXTURES) + "/physics2d/legacy_run.csv";
    }

    Entity legacyEntity(Scene& scene, const Vector3& position, float rollDegrees = 0.0f) {
        Entity entity = scene.createEntity();
        Transform transform;
        transform.position = position;
        if (rollDegrees != 0.0f) {
            transform.rotation = Quaternion(rollDegrees, Vector3(0, 0, 1));
        }
        scene.addComponent<Transform>(entity, transform);
        return entity;
    }

    // a collider block as the editor wrote it before (numColliders, vertices, every flag)
    std::string legacyCollider(const std::string& type, const std::string& pointA, const std::string& pointB, float radius,
                               const std::string& vertices, float density, float friction, float restitution) {
        std::ostringstream yaml;
        yaml << "  - type: " << type << "\n"
             << "    pointA: " << pointA << "\n"
             << "    pointB: " << pointB << "\n"
             << "    radius: " << radius << "\n"
             << "    loop: false\n"
             << "    density: " << density << "\n"
             << "    friction: " << friction << "\n"
             << "    restitution: " << restitution << "\n"
             << "    sensor: false\n"
             << "    enableHitEvents: false\n"
             << "    contactEvents: false\n"
             << "    preSolveEvents: false\n"
             << "    sensorEvents: false\n"
             << "    categoryBits: 1\n"
             << "    maskBits: 65535\n"
             << "    groupIndex: 0\n"
             << "    vertices: " << vertices << "\n";
        return yaml.str();
    }

    std::string legacyBody(const std::string& type, const std::vector<std::string>& colliders) {
        std::ostringstream yaml;
        yaml << "enabled: true\n"
             << "type: " << type << "\n"
             << "numColliders: " << colliders.size() << "\n"
             << "colliders:\n";
        for (const std::string& collider : colliders) yaml << collider;
        return yaml.str();
    }

    std::string legacyJoint(const std::string& type, Entity bodyA, Entity bodyB, const std::string& anchorA,
                            const std::string& anchorB, const std::string& axis, bool autoAnchors, bool rope) {
        std::ostringstream yaml;
        yaml << "enabled: true\n"
             << "type: " << type << "\n"
             << "bodyA: " << bodyA << "\n"
             << "bodyB: " << bodyB << "\n"
             << "anchorA: " << anchorA << "\n"
             << "anchorB: " << anchorB << "\n"
             << "axis: " << axis << "\n"
             << "target: [0, 0]\n"
             << "autoAnchors: " << (autoAnchors ? "true" : "false") << "\n"
             << "rope: " << (rope ? "true" : "false") << "\n";
        return yaml.str();
    }

    void addBody(Scene& scene, Entity entity, const std::string& yaml) {
        scene.addComponent<Rigidbody2DComponent>(entity, Stream::decodeRigidbody2DComponent(YAML::Load(yaml)));
    }

    void addJoint(Scene& scene, Entity entity, const std::string& yaml) {
        scene.addComponent<Joint2DComponent>(entity, Stream::decodeJoint2DComponent(YAML::Load(yaml)));
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

    // The legacy scene: a car (box body + wheel on a WHEEL joint, world anchor at the wheel
    // centre, axis up) dropped on a static ground, a pendulum on a REVOLUTE joint at a world
    // point, and a ball on a DISTANCE rope from a static hook, with explicit world anchors.
    // Returns, every 10 steps, the pose of each dynamic body: "step,body,x,y,angle".
    std::vector<std::string> runLegacyScene() {
        Scene scene;
        scene.setPhysicsInterpolation(false);

        const Entity ground = legacyEntity(scene, Vector3(0, -40, 0));
        addBody(scene, ground, legacyBody("static", {
            legacyCollider("polygon", "[-800, -20]", "[800, 20]", 0, "[[-800, -20], [800, -20], [800, 20], [-800, 20]]", 1, 0.6f, 0)}));

        const Entity car = legacyEntity(scene, Vector3(0, 80, 0));
        addBody(scene, car, legacyBody("dynamic", {
            legacyCollider("polygon", "[-60, -15]", "[60, 15]", 0, "[[-60, -15], [60, -15], [60, 15], [-60, 15]]", 1, 0.6f, 0)}));

        const Entity wheel = legacyEntity(scene, Vector3(45, 50, 0));
        addBody(scene, wheel, legacyBody("dynamic", {
            legacyCollider("circle", "[0, 0]", "[0, 0]", 18, "[]", 1, 0.9f, 0)}));

        const Entity rearWheel = legacyEntity(scene, Vector3(-45, 50, 0));
        addBody(scene, rearWheel, legacyBody("dynamic", {
            legacyCollider("circle", "[0, 0]", "[0, 0]", 18, "[]", 1, 0.9f, 0)}));

        const Entity wheelJoint = legacyEntity(scene, Vector3(0, 0, 0));
        addJoint(scene, wheelJoint, legacyJoint("wheel", car, wheel, "[45, 50]", "[0, 0]", "[0, 1]", true, false));
        const Entity rearWheelJoint = legacyEntity(scene, Vector3(0, 0, 0));
        addJoint(scene, rearWheelJoint, legacyJoint("wheel", car, rearWheel, "[-45, 50]", "[0, 0]", "[0, 1]", true, false));

        const Entity pivot = legacyEntity(scene, Vector3(300, 300, 0));
        addBody(scene, pivot, legacyBody("static", {
            legacyCollider("circle", "[0, 0]", "[0, 0]", 4, "[]", 1, 0.6f, 0)}));
        const Entity bob = legacyEntity(scene, Vector3(380, 300, 0), 30.0f);
        addBody(scene, bob, legacyBody("dynamic", {
            legacyCollider("polygon", "[-10, -10]", "[10, 10]", 0, "[[-10, -10], [10, -10], [10, 10], [-10, 10]]", 2, 0.6f, 0)}));
        const Entity hinge = legacyEntity(scene, Vector3(0, 0, 0));
        addJoint(scene, hinge, legacyJoint("revolute", pivot, bob, "[300, 300]", "[0, 0]", "[0, 0]", true, false));

        const Entity hook = legacyEntity(scene, Vector3(-300, 300, 0));
        addBody(scene, hook, legacyBody("static", {
            legacyCollider("circle", "[0, 0]", "[0, 0]", 4, "[]", 1, 0.6f, 0)}));
        const Entity ball = legacyEntity(scene, Vector3(-240, 260, 0));
        addBody(scene, ball, legacyBody("dynamic", {
            legacyCollider("circle", "[0, 0]", "[0, 0]", 10, "[]", 1, 0.6f, 0.2f)}));
        const Entity rope = legacyEntity(scene, Vector3(0, 0, 0));
        addJoint(scene, rope, legacyJoint("distance", hook, ball, "[-300, 300]", "[-240, 260]", "[0, 0]", false, true));

        resolveTransforms(scene);
        scene.getSystem<PhysicsSystem>()->load();

        const std::vector<std::pair<std::string, Entity>> dynamics = {
            {"car", car}, {"wheel", wheel}, {"rearWheel", rearWheel}, {"bob", bob}, {"ball", ball}};

        std::vector<std::string> rows;
        for (int stepIndex = 1; stepIndex <= 120; stepIndex++) {
            scene.getSystem<PhysicsSystem>()->fixedUpdate(legacyStep);
            resolveTransforms(scene);
            if (stepIndex % 10 != 0) continue;
            for (const auto& [name, entity] : dynamics) {
                Rigidbody2D body(&scene, entity);
                const Vector2 position = body.getPosition();
                char row[160];
                std::snprintf(row, sizeof(row), "%d,%s,%.6f,%.6f,%.6f", stepIndex, name.c_str(), position.x, position.y, body.getAngle());
                rows.push_back(row);
            }
        }
        return rows;
    }

}

TEST(Physics2DLegacy, OldSceneSimulatesAsRecordedOnMain) {
    const std::vector<std::string> rows = runLegacyScene();

    if (const char* record = std::getenv("JASPER_RECORD_PHYSICS2D_FIXTURE"); record && std::string(record) == "1") {
        std::ofstream out(fixturePath());
        ASSERT_TRUE(out.good()) << fixturePath();
        out << "step,body,x,y,angle\n";
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
        float xA, yA, angleA, xB, yB, angleB;
        ASSERT_EQ(std::sscanf(line.c_str(), "%d,%31[^,],%f,%f,%f", &stepA, nameA, &xA, &yA, &angleA), 5) << line;
        ASSERT_EQ(std::sscanf(rows[index].c_str(), "%d,%31[^,],%f,%f,%f", &stepB, nameB, &xB, &yB, &angleB), 5) << rows[index];
        EXPECT_EQ(stepA, stepB);
        EXPECT_STREQ(nameA, nameB);
        // poses in points (64 per metre): 1e-5 metres
        EXPECT_NEAR(xA, xB, 64e-5f) << line << " vs " << rows[index];
        EXPECT_NEAR(yA, yB, 64e-5f) << line << " vs " << rows[index];
        EXPECT_NEAR(angleA, angleB, 1e-3f) << line << " vs " << rows[index];
        index++;
    }
    EXPECT_EQ(index, rows.size());
}
