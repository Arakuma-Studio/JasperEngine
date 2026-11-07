// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Instanced Mesh's camera-bound instances and distance fade
// (docs/superpowers/specs/2026-11-04-render-correctness-design.md 6.7):
// - an opaque billboard on the main camera kept its first rotation: only transparent sorting and
//   camera-dependent particles rebuilt on a camera move (Doriax e3aa4e1b8);
// - Cylindrical Billboard was stored, shown and scripted but never drawn; under a non-uniform
//   scale its face turns toward the camera's side once scaled;
// - any horizontal eye move rebuilt a fading batch's lists and buffers; they are cut past a
//   margin the fade has already shrunk to nothing, so small moves rebuild nothing;
// - the near fade (Near Fade Start/End, a far level of detail's inner edge) shrinks and cuts the
//   instances inside it, and the fade cuts and shrinks under SSR too (its G-buffer has it).
// glcore and Vulkan.

#include "Engine.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Primitive.h"
#include "pool/ShaderPool.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class InstancedFadeGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};
        Primitive batch{&scene};

        void SetUp() override{
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 2.0f, 6.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(0.0f, -1.0f, -0.3f);
            sun.setShadows(false);
            batch.createPlane(1.0f, 1.0f);
            batch.createInstancedMesh();
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
        }

        void settle(){
            Scene* current = &scene;
            EXPECT_TRUE(GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            })) << "the scene never finished loading";
            GpuEnvironment::drawFrames(2);
        }

        InstancedMeshComponent& instances(){
            return scene.getComponent<InstancedMeshComponent>(batch.getEntity());
        }

        // the main camera's rotation in the world (the batch's entity has none)
        Quaternion cameraRotation(){
            Quaternion rotation;
            rotation.fromRotationMatrix(scene.getComponent<CameraComponent>(camera.getEntity()).viewMatrix.inverse());
            return rotation;
        }

        // a column of the first render instance's matrix (0 x, 1 y, 2 z), unit length
        Vector3 instanceAxis(int column){
            const Matrix4& m = instances().renderInstances.at(0).instanceMatrix;
            return Vector3(m[column][0], m[column][1], m[column][2]).normalized();
        }
    };

    // the pixels of the target the batch covers (anything brighter than the black background)
    int fadeCoveredPixels(){
        Framebuffer& target = GpuEnvironment::target();
        std::vector<uint8_t> rgba;
        readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
        int covered = 0;
        for (size_t i = 0; i + 3 < rgba.size(); i += 4)
            covered += (rgba[i] + rgba[i + 1] + rgba[i + 2] > 30) ? 1 : 0;
        return covered;
    }

    void expectAxisNear(const Vector3& actual, const Vector3& expected, const char* what){
        EXPECT_NEAR(actual.x, expected.x, 1e-3f) << what;
        EXPECT_NEAR(actual.y, expected.y, 1e-3f) << what;
        EXPECT_NEAR(actual.z, expected.z, 1e-3f) << what;
    }

}

TEST_F(InstancedFadeGpu, AnOpaqueBillboardFollowsTheCameraRotation){
    JASPER_REQUIRE_GPU();
    batch.addInstance(Vector3(0.0f, 0.0f, 0.0f));
    batch.setInstancedBillboard(true);
    settle();
    ASSERT_FALSE(instances().renderInstances.empty());
    expectAxisNear(instanceAxis(2), cameraRotation() * Vector3(0.0f, 0.0f, 1.0f), "the billboard does not face the camera");

    // the camera turns where it stands
    camera.setTarget(3.0f, 0.0f, 0.0f);
    GpuEnvironment::drawFrames(2);
    expectAxisNear(instanceAxis(2), cameraRotation() * Vector3(0.0f, 0.0f, 1.0f), "the opaque billboard kept its old rotation");
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(InstancedFadeGpu, CylindricalBillboardTurnsAboutUpOnly){
    JASPER_REQUIRE_GPU();
    batch.addInstance(Vector3(0.0f, 0.0f, 0.0f));
    batch.setInstancedBillboard(true, true);
    camera.setPosition(4.0f, 4.0f, 0.0f);
    settle();
    ASSERT_FALSE(instances().renderInstances.empty());
    // upright, its face toward the camera's side
    expectAxisNear(instanceAxis(1), Vector3(0.0f, 1.0f, 0.0f), "the cylindrical billboard tilts");
    expectAxisNear(instanceAxis(2), Vector3(1.0f, 0.0f, 0.0f), "the cylindrical billboard does not face the camera's side");

    // straight above, looking down: no side to face, it keeps its yaw
    camera.setPosition(0.0f, 6.0f, 0.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.setUp(0.0f, 0.0f, -1.0f);
    GpuEnvironment::drawFrames(2);
    expectAxisNear(instanceAxis(1), Vector3(0.0f, 1.0f, 0.0f), "the cylindrical billboard tilts under a camera above it");
    expectAxisNear(instanceAxis(2), Vector3(1.0f, 0.0f, 0.0f), "a camera straight above changed the yaw");
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(InstancedFadeGpu, CylindricalBillboardFacesTheCameraUnderAScale){
    JASPER_REQUIRE_GPU();
    batch.addInstance(Vector3(0.0f, 0.0f, 0.0f));
    batch.setInstancedBillboard(true, true);
    const Vector3 scale(2.0f, 1.0f, 0.5f);
    batch.setScale(scale);
    camera.setPosition(4.0f, 4.0f, 4.0f);
    settle();
    ASSERT_FALSE(instances().renderInstances.empty());
    // the drawn face's normal: the cross of its scaled x and y axes
    const Matrix4& m = instances().renderInstances.at(0).instanceMatrix;
    const Vector3 x(m[0][0] * scale.x, m[0][1] * scale.y, m[0][2] * scale.z);
    const Vector3 y(m[1][0] * scale.x, m[1][1] * scale.y, m[1][2] * scale.z);
    expectAxisNear(x.crossProduct(y).normalized(), Vector3(1.0f, 0.0f, 1.0f).normalized(), "the scaled cylindrical billboard does not face the camera's side");
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(InstancedFadeGpu, SubMarginMovesDoNotRebuild){
    JASPER_REQUIRE_GPU();
    for (int i = 0; i < 8; i++)
        batch.addInstance(Vector3((float)i * 3.0f, 0.0f, 0.0f));
    batch.setDistanceFade(true);
    batch.setFadeRange(10.0f, 20.0f);
    settle();
    const Vector3 cutEye = instances().lastFadeCullEye;
    // the margin: max(0.25, 0.04 * (20 - 10)) = 0.4
    EXPECT_NEAR(instances().lastCullSlack, 0.4f, 1e-5f);

    camera.setPosition(0.1f, 2.0f, 6.0f);
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(instances().lastFadeCullEye.x, cutEye.x) << "a move inside the margin rebuilt the lists";

    camera.setPosition(1.0f, 2.0f, 6.0f);
    GpuEnvironment::drawFrames(2);
    EXPECT_NE(instances().lastFadeCullEye.x, cutEye.x) << "a move past the margin did not cut the lists again";
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(InstancedFadeGpu, NearFadeCutsTheInstancesInsideIt){
    JASPER_REQUIRE_GPU();
    // the eye at (0, 0) in the batch's space: 1 m and 12 m away
    camera.setPosition(0.0f, 2.0f, 0.01f);
    batch.addInstance(Vector3(1.0f, 0.0f, 0.0f));
    batch.addInstance(Vector3(12.0f, 0.0f, 0.0f));
    batch.setDistanceFade(true);
    batch.setFadeRange(30.0f, 40.0f);
    batch.setNearFadeRange(4.0f, 5.0f);
    settle();
    EXPECT_EQ(instances().numVisible, 1u) << "the instance inside the near fade is drawn";

    batch.setNearFadeRange(0.0f, 0.0f);
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(instances().numVisible, 2u) << "an empty near range still cuts";
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(InstancedFadeGpu, TheFadeCutsAndShrinksUnderSsr){
    JASPER_REQUIRE_GPU();
    scene.setSSREnabled(true);
    camera.setPosition(0.0f, 2.0f, 0.01f);
    batch.addInstance(Vector3(1.0f, 0.0f, 0.0f));
    batch.addInstance(Vector3(25.0f, 0.0f, 0.0f));
    batch.setDistanceFade(true);
    batch.setFadeRange(10.0f, 20.0f);
    settle();
    EXPECT_EQ(instances().numVisible, 1u) << "SSR turned the distance cut off";
    const Submesh& submesh = scene.getComponent<MeshComponent>(batch.getEntity()).submeshes[0];
    ASSERT_TRUE(submesh.gbufferShader) << "SSR on draws no G-buffer";
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::GBUFFER, 18, true), "Ifd");
    EXPECT_NE(submesh.gbufferShaderProperties & (uint64_t(1) << 18), 0u) << "the G-buffer does not shrink faded instances ('Ifd')";
    EXPECT_NO_ENGINE_ERRORS();
}

// inside the near band an instance is drawn shrunk toward its base: half way through, at half size
TEST_F(InstancedFadeGpu, TheNearBandShrinksTheInstance){
    JASPER_REQUIRE_GPU();
    scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
    scene.setGlobalIllumination(1.0f);
    camera.setPosition(0.0f, 8.0f, 0.01f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    batch.setFaceCulling(false);
    batch.addInstance(Vector3(3.0f, 0.0f, 0.0f));
    batch.setDistanceFade(true);
    batch.setFadeRange(30.0f, 40.0f);
    settle();
    const int whole = fadeCoveredPixels();
    ASSERT_GT(whole, 50) << "the instance does not draw";

    // the instance 3 m away, the band from 2 m to 4 m: half its size, a quarter of its area
    batch.setNearFadeRange(2.0f, 4.0f);
    GpuEnvironment::drawFrames(3);
    const int band = fadeCoveredPixels();
    EXPECT_GT(band, whole / 10) << "the instance half way through the band vanished";
    EXPECT_LT(band, whole / 2) << "the instance half way through the band is not shrunk";
    EXPECT_NO_ENGINE_ERRORS();
}
