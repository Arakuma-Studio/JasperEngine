// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The scene's default shader property values reach the GPU (docs/superpowers/specs/
// 2026-11-04-code-editor-git-design.md 6.6, AC12): a plane drawn with the scene's default mesh
// shader (library:unlit) takes the scene's Base colour; its own value overrides it; without
// either it is the shader's default (white).

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/Primitive.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class SceneDefaultUniformsGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override{
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 4.0f, 0.01f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
        }

        // the centre pixel after the scene settled (rgba, 0..255)
        std::vector<int> centre(){
            Scene* current = &scene;
            EXPECT_TRUE(GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); }));
            Framebuffer& target = GpuEnvironment::target();
            std::vector<uint8_t> rgba;
            EXPECT_TRUE(readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba));
            const size_t p = ((size_t)(target.getHeight() / 2) * target.getWidth() + target.getWidth() / 2) * 4;
            if (rgba.size() < p + 4)
                return {0, 0, 0, 0};
            return {rgba[p], rgba[p + 1], rgba[p + 2], rgba[p + 3]};
        }
    };

}

TEST_F(SceneDefaultUniformsGpu, InheritedBaseColour){
    JASPER_REQUIRE_GPU();
    Primitive plane(&scene);
    plane.createPlane(6.0f, 6.0f);
    plane.setReceiveLights(false);
    scene.setDefaultMeshShader("library:unlit");

    // the scene's value: red
    scene.setDefaultShaderUniform(ShaderType::MESH, "baseColor", Vector4(1.0f, 0.0f, 0.0f, 1.0f));
    std::vector<int> pixel = centre();
    EXPECT_GT(pixel[0], 200);
    EXPECT_LT(pixel[1], 40);
    EXPECT_LT(pixel[2], 40);

    // the plane's own value wins: green
    plane.setShaderUniform("baseColor", Vector4(0.0f, 1.0f, 0.0f, 1.0f));
    pixel = centre();
    EXPECT_LT(pixel[0], 40);
    EXPECT_GT(pixel[1], 200);

    // neither: the shader's default, white
    plane.removeShaderUniform("baseColor");
    scene.removeDefaultShaderUniform(ShaderType::MESH, "baseColor");
    pixel = centre();
    EXPECT_GT(pixel[0], 200);
    EXPECT_GT(pixel[1], 200);
    EXPECT_GT(pixel[2], 200);
    EXPECT_NO_ENGINE_ERRORS();
}
