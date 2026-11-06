// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A camera that looks at its target straight down or up with the default up vector (0, 1, 0):
// lookAt's side vector, view x up, is zero there, so the view matrix had a zero row and drew
// nothing (agent-authoring: set_scene_camera's top-down shots, a game's top-down camera).
// cameraUpFor picks the Top/Bottom axis view's up instead.

#include <gtest/gtest.h>

#include "Engine.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/Transform.h"
#include "subsystem/RenderSystem.h"

#include <cmath>

using namespace jasperengine;

namespace {

    class CameraLook : public ::testing::Test {
    protected:
        void SetUp() override { Engine::setScene(&scene); }
        void TearDown() override { Engine::removeScene(&scene); }

        // a target camera at position looking at target, up (0, 1, 0), as RenderSystem updates it
        Matrix4 view(const Vector3& position, const Vector3& target){
            Entity entity = scene.createEntity();
            Transform transform;
            transform.position = position;
            scene.addComponent<Transform>(entity, transform);
            CameraComponent camera;
            camera.type = CameraType::CAMERA_PERSPECTIVE;
            camera.useTarget = true;
            camera.target = target;
            camera.up = Vector3(0, 1, 0);
            scene.addComponent<CameraComponent>(entity, camera);
            RenderSystem& render = *scene.getSystem<RenderSystem>();
            Transform& stored = scene.getComponent<Transform>(entity);
            render.updateTransform(stored, entity);
            render.updateCamera(scene.getComponent<CameraComponent>(entity), stored);
            return scene.getComponent<CameraComponent>(entity).viewMatrix;
        }

        // the rotation part's rows are unit length and orthogonal (a rotation, no collapsed axis)
        static void expectRotation(const Matrix4& m){
            for (int r = 0; r < 3; r++){
                const Vector3 row(m[0][r], m[1][r], m[2][r]);
                EXPECT_NEAR(row.length(), 1.0f, 1e-4f) << "row " << r;
                for (int other = r + 1; other < 3; other++){
                    const Vector3 second(m[0][other], m[1][other], m[2][other]);
                    EXPECT_NEAR(row.dotProduct(second), 0.0f, 1e-4f) << "rows " << r << " " << other;
                }
            }
        }

        Scene scene;
    };

}

TEST_F(CameraLook, StraightDownIsAValidView){
    const Matrix4 m = view(Vector3(0, 10, 0), Vector3(0, 0, 0));
    expectRotation(m);
    // the target is in front of the camera: on the view's -Z axis, 10 units away
    const Vector3 target = m * Vector3(0, 0, 0);
    EXPECT_NEAR(target.x, 0.0f, 1e-4f);
    EXPECT_NEAR(target.y, 0.0f, 1e-4f);
    EXPECT_NEAR(target.z, -10.0f, 1e-3f);
}

TEST_F(CameraLook, StraightUpIsAValidView){
    const Matrix4 m = view(Vector3(0, -5, 0), Vector3(0, 0, 0));
    expectRotation(m);
    EXPECT_NEAR((m * Vector3(0, 0, 0)).z, -5.0f, 1e-3f);
}

TEST(CameraUpFor, KeepsTheUpUnlessParallel){
    EXPECT_EQ(cameraUpFor(Vector3(1, -1, 0), Vector3(0, 1, 0)), Vector3(0, 1, 0));
    EXPECT_EQ(cameraUpFor(Vector3(0, -1, 0), Vector3(0, 1, 0)), Vector3(0, 0, 1));
    EXPECT_EQ(cameraUpFor(Vector3(0, 3, 0), Vector3(0, 1, 0)), Vector3(0, 0, -1));
    // a direction a little off parallel keeps the up: only exactly parallel vectors degenerate
    EXPECT_EQ(cameraUpFor(Vector3(0.001f, -1, 0), Vector3(0, 1, 0)), Vector3(0, 1, 0));
    // a zero direction (target on the camera) keeps the up: there is nothing to look along
    EXPECT_EQ(cameraUpFor(Vector3(0, 0, 0), Vector3(0, 1, 0)), Vector3(0, 1, 0));
}

// the orbit clamp (Camera::ELEVATION_DOT_LIMIT, 1 degree from vertical): an orbit dragged to its top
// keeps world up, or the view would roll as it reaches the clamp
TEST(CameraUpFor, OrbitClampKeepsWorldUp){
    const float angle = 1.0f * 3.14159265f / 180.0f;
    EXPECT_EQ(cameraUpFor(Vector3(std::sin(angle), -std::cos(angle), 0), Vector3(0, 1, 0)), Vector3(0, 1, 0));
    EXPECT_EQ(cameraUpFor(Vector3(0, std::cos(angle), std::sin(angle)), Vector3(0, 1, 0)), Vector3(0, 1, 0));
}

// the fallback is perpendicular to the view: with up (0, 0, 1) and a view along Z it is not (0, 0, -1)
TEST(CameraUpFor, FallbackIsPerpendicularToTheView){
    for (const Vector3& direction : {Vector3(0, 0, 1), Vector3(0, 0, -1), Vector3(1, 0, 0), Vector3(0, -1, 0), Vector3(0, 1, 0)}){
        for (const Vector3& up : {Vector3(0, 0, 1), Vector3(1, 0, 0), Vector3(0, 1, 0)}){
            if (direction.crossProduct(up).length() > 0.0f) continue;   // only the parallel pairs
            const Vector3 chosen = cameraUpFor(direction, up);
            EXPECT_NEAR(chosen.dotProduct(direction), 0.0f, 1e-6f) << "view (" << direction.x << ", " << direction.y << ", " << direction.z << ")";
            EXPECT_NEAR(chosen.length(), 1.0f, 1e-6f);
        }
    }
    EXPECT_EQ(cameraUpFor(Vector3(0, 0, 1), Vector3(0, 0, 1)), Vector3(0, 1, 0));
    EXPECT_EQ(cameraUpFor(Vector3(0, 0, -1), Vector3(0, 0, 1)), Vector3(0, 1, 0));
}
