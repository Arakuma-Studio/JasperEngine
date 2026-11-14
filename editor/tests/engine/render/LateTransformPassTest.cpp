// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The late transform pass (S95 REQ-5, AC-8): a camera follows a target the script moves in
// onUpdate. When the camera moves after the scenes' update (onPostUpdate, a timer, a
// coroutine), the frame still ends with its view matrix, and its child's model matrix, at the
// pose set in that frame; a skin's joint moved in onPostUpdate poses the skin that frame too.
// Before the pass they were one frame behind.

#include "ScriptTestHarness.h"

#include "component/CameraComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "subsystem/RenderSystem.h"

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
    EXPECT_FALSE(scene->getSystem<RenderSystem>()->updateLateTransforms());
    const Vector3 eye = matrixOrigin(scene->getComponent<CameraComponent>(camera).viewMatrix.inverse());
    EXPECT_NEAR(eye.z, 0.0f, 1e-4f) << "the camera never moved";
}
