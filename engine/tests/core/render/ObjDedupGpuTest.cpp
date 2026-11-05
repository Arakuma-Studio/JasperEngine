// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// An OBJ draws the same after its corners are welded and reordered (docs/specs/S53-model-import-cook.md
// R7, AC4): the golden obj_dedup_sphere was drawn by the loader of before the weld (one vertex per
// face corner, faces in file order), and the welded mesh must match it. glcore and Vulkan.

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Model.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::test;

TEST(ObjDedupGpu, WeldedSphereDrawsAsBefore){
    JASPER_REQUIRE_GPU();
    Scene scene;
    Camera camera(&scene);
    scene.setBackgroundColor(0.12f, 0.12f, 0.16f, 1.0f);
    camera.setType(CameraType::CAMERA_PERSPECTIVE);
    camera.setPosition(0.0f, 1.2f, 3.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();
    Engine::setScene(&scene);
    {
        Light sun(&scene);
        sun.setType(LightType::DIRECTIONAL);
        sun.setDirection(-0.4f, -1.0f, -0.6f);
        sun.setIntensity(1.5f);
        sun.setShadows(false);
        Model sphere(&scene);
        ASSERT_TRUE(sphere.loadModel(JASPER_TEST_REPO_DIR "/engine/tests/core/fixtures/gltf/s41/dedup_sphere.obj"));
        Scene* current = &scene;
        ASSERT_TRUE(GpuEnvironment::runFrames([current](){
            return current->getSystem<RenderSystem>()->isAllLoaded();
        })) << "the scene never finished loading";
        EXPECT_GOLDEN("obj_dedup_sphere");
        EXPECT_NO_ENGINE_ERRORS();
    }
    Engine::removeScene(&scene);
}
