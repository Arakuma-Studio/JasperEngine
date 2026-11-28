// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The late transform pass (S95 REQ-5, AC-8): a camera follows a target the script moves in
// onUpdate. When the camera moves after the scenes' update (onPostUpdate, a timer, a
// coroutine), the frame still ends with its view matrix, and its child's model matrix, at the
// pose set in that frame; a skin's joint moved in onPostUpdate poses the skin that frame too; a
// Camera Follow follows a target moved in onPostUpdate in that frame, damped as if the target had
// moved in onUpdate. Before the pass they were one frame behind.

#include "ScriptTestHarness.h"

#include "component/CameraComponent.h"
#include "component/CameraFollowComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "subsystem/RenderSystem.h"

namespace LateTransformPassTest_cpp {

using namespace jasperengine;
using namespace jasperengine::scripttest;

namespace {

    const char* const FOLLOW = R"(
local Follow = {}
function Follow:init()
    RegisterEngineEvent(self, "onUpdate")
    RegisterEngineEvent(self, "onPostUpdate")
    self.target = GameObject(self.scene, TARGET)
    if followMode == "timer" then
        Timer.every(0.001, function() self:follow() end, { owner = self })
    elseif followMode == "coroutine" then
        self:startCoroutine(function()
            while true do
                self:follow()
                Coroutine.nextFrame()
            end
        end)
    end
end
function Follow:onUpdate()
    followFrame = (followFrame or 0) + 1
    self.target.position = Vector3(followFrame, 2 * followFrame, 0)
end
function Follow:onPostUpdate()
    if followMode == "post" then self:follow() end
    if JOINT then GameObject(self.scene, JOINT).position = Vector3(followFrame, 0, 0) end
end
-- ten units behind the target, as the target is now
function Follow:follow()
    self.gameObject.position = self.target.position + Vector3(0, 0, 10)
end
return Follow
)";

    // moves its GameObject to (frame, 2 * frame, 0) in onUpdate or in onPostUpdate (moverMode)
    const char* const MOVER = R"(
local Mover = {}
function Mover:init()
    RegisterEngineEvent(self, "onUpdate")
    RegisterEngineEvent(self, "onPostUpdate")
    self.frame = 0
end
function Mover:onUpdate()
    self.frame = self.frame + 1
    if moverMode == "update" then self:move() end
end
function Mover:onPostUpdate()
    if moverMode == "post" then self:move() end
end
function Mover:move()
    self.gameObject.position = Vector3(self.frame, 2 * self.frame, 0)
end
return Mover
)";

    Vector3 matrixOrigin(const Matrix4& matrix) {
        return matrix * Vector3(0, 0, 0);
    }

    class LateTransformPassTest : public ScriptTest {
    protected:
        Scene* scene = nullptr;
        Entity camera = NULL_ENTITY;
        Entity rig = NULL_ENTITY;

        void build(const std::string& mode) {
            writeScript("Follow", FOLLOW);
            scene = newScene();
            const Entity target = entity(scene, "Target");
            camera = entity(scene, "Camera");
            rig = entity(scene, "Rig", camera);
            scene->getComponent<Transform>(rig).position = Vector3(0, 1, 0);
            CameraComponent cameraComponent;
            cameraComponent.type = CameraType::CAMERA_PERSPECTIVE;
            cameraComponent.useTarget = false;
            scene->addComponent<CameraComponent>(camera, cameraComponent);
            scene->setCamera(camera);
            addScript(scene, camera, "Follow");
            lua("followMode = '" + mode + "'; followFrame = nil; TARGET = " + std::to_string(target));
            Engine::setScene(scene);
            LuaBinding::initializeLuaScripts(scene);
        }

        // after frame f the camera is at (f, 2f, 10), its Rig one unit above
        void expectFollowing(int frames) {
            for (int f = 1; f <= frames; f++) {
                frame();
                SCOPED_TRACE("frame " + std::to_string(f));
                ASSERT_EQ(lua("return followFrame"), std::to_string(f));
                const CameraComponent& view = scene->getComponent<CameraComponent>(camera);
                const Vector3 eye = matrixOrigin(view.viewMatrix.inverse());
                EXPECT_NEAR(eye.x, (float)f, 1e-4f);
                EXPECT_NEAR(eye.y, 2.0f * f, 1e-4f);
                EXPECT_NEAR(eye.z, 10.0f, 1e-4f);
                const Vector3 child = matrixOrigin(scene->getComponent<Transform>(rig).modelMatrix);
                EXPECT_NEAR(child.x, (float)f, 1e-4f);
                EXPECT_NEAR(child.y, 2.0f * f + 1.0f, 1e-4f);
                EXPECT_NEAR(child.z, 10.0f, 1e-4f);
            }
        }

        void TearDown() override {
            lua("followMode = nil; followFrame = nil; TARGET = nil; JOINT = nil");
            ScriptTest::TearDown();
        }
    };

}

namespace {

    // a camera with a Camera Follow (offset (0, 0, 10), no aim) of a target its Mover moves
    class LateFollowTest : public ScriptTest {
    protected:
        // the camera's eye after each of `frames` frames
        std::vector<Vector3> followEyes(const std::string& mode, float damping, int frames) {
            writeScript("Mover", MOVER);
            Scene* scene = newScene();
            const Entity target = entity(scene, "Target");
            const Entity camera = entity(scene, "Camera");
            CameraComponent cameraComponent;
            cameraComponent.type = CameraType::CAMERA_PERSPECTIVE;
            cameraComponent.useTarget = false;
            scene->addComponent<CameraComponent>(camera, cameraComponent);
            scene->setCamera(camera);
            CameraFollowComponent follow;
            follow.target = target;
            follow.offset = Vector3(0, 0, 10);
            follow.damping = Vector3(damping, damping, damping);
            follow.lookAt = false;
            scene->addComponent<CameraFollowComponent>(camera, follow);
            addScript(scene, target, "Mover");
            lua("moverMode = '" + mode + "'");
            Engine::setScene(scene);
            LuaBinding::initializeLuaScripts(scene);
            std::vector<Vector3> eyes;
            for (int f = 0; f < frames; f++) {
                frame();
                eyes.push_back(matrixOrigin(scene->getComponent<CameraComponent>(camera).viewMatrix.inverse()));
            }
            Engine::removeAllScenes();
            LuaBinding::cleanupLuaScripts(scene);
            return eyes;
        }

        void TearDown() override {
            lua("moverMode = nil");
            ScriptTest::TearDown();
        }
    };

}

// the target moves in onPostUpdate: a rigid follower is at it in that frame
TEST_F(LateFollowTest, TargetMovedInPostUpdateIsFollowedThisFrame) {
    const std::vector<Vector3> eyes = followEyes("post", 0.0f, 4);
    for (int f = 1; f <= 4; f++) {
        SCOPED_TRACE("frame " + std::to_string(f));
        EXPECT_NEAR(eyes[f - 1].x, (float)f, 1e-4f);
        EXPECT_NEAR(eyes[f - 1].y, 2.0f * f, 1e-4f);
        EXPECT_NEAR(eyes[f - 1].z, 10.0f, 1e-4f);
    }
}

// damped: each frame ends where it ends when the target moves in onUpdate (one step a frame, from
// the same start, with the same delta)
TEST_F(LateFollowTest, DampedFollowOfALateTargetMatchesAnOnUpdateTarget) {
    const std::vector<Vector3> early = followEyes("update", 0.25f, 6);
    const std::vector<Vector3> late = followEyes("post", 0.25f, 6);
    ASSERT_EQ(early.size(), late.size());
    for (size_t f = 0; f < early.size(); f++) {
        SCOPED_TRACE("frame " + std::to_string(f + 1));
        EXPECT_NEAR(late[f].x, early[f].x, 1e-5f);
        EXPECT_NEAR(late[f].y, early[f].y, 1e-5f);
        EXPECT_NEAR(late[f].z, early[f].z, 1e-5f);
    }
    EXPECT_GT(early.back().x, 1.0f) << "it did follow";
}

TEST_F(LateTransformPassTest, CameraMovedInPostUpdateDrawsThisFrame) {
    build("post");
    expectFollowing(4);
}

TEST_F(LateTransformPassTest, CameraMovedInTimerDrawsThisFrame) {
    build("timer");
    expectFollowing(4);
}

TEST_F(LateTransformPassTest, CameraMovedInCoroutineDrawsThisFrame) {
    build("coroutine");
    expectFollowing(4);
}

// a skin whose joint moves in onPostUpdate: its bone matrix is the joint's pose of that frame
TEST_F(LateTransformPassTest, SkinJointMovedInPostUpdatePosesThisFrame) {
    build("none");
    const Entity skinned = entity(scene, "Skinned");
    const Entity joint = entity(scene, "Joint");
    MeshComponent mesh;
    mesh.loadCalled = true;   // nothing to load: the bones are what is checked
    mesh.bonesMatrix.resize(1);
    scene->addComponent<MeshComponent>(skinned, mesh);
    ModelComponent model;
    model.needUpdateModel = false;
    ModelSkinBinding binding;
    binding.mesh = skinned;
    binding.joints = {joint};
    binding.inverseBindMatrices = {Matrix4()};
    model.skinBindings.push_back(binding);
    scene->addComponent<ModelComponent>(skinned, model);
    lua("JOINT = " + std::to_string(joint));

    for (int f = 1; f <= 3; f++) {
        frame();
        SCOPED_TRACE("frame " + std::to_string(f));
        const MeshComponent& posed = scene->getComponent<MeshComponent>(skinned);
        ASSERT_TRUE(posed.bonesMatrix.validIndex(0));
        EXPECT_NEAR(matrixOrigin(posed.bonesMatrix[0]).x, (float)f, 1e-4f);
    }
}

// nothing moved after the update: the pass does nothing (one compare)
TEST_F(LateTransformPassTest, NothingMovedLateIsAPassThrough) {
    build("none");
    frame();
    frame();
    // the target moved in onUpdate only: the scenes' update drew it, the late pass had no work
    EXPECT_FALSE(scene->getSystem<RenderSystem>()->updateLateTransforms().moved);
    const Vector3 eye = matrixOrigin(scene->getComponent<CameraComponent>(camera).viewMatrix.inverse());
    EXPECT_NEAR(eye.z, 0.0f, 1e-4f) << "the camera never moved";
}

}  // namespace LateTransformPassTest_cpp
