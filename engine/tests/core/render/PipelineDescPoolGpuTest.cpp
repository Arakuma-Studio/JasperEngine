// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The pipeline descriptions of render objects (mesh-memory spec 6.5): a load's working
// description lives from beginLoad to endLoad only, and objects loaded with an equal base
// description (shader, vertex layout, index and primitive type) share one. glcore and Vulkan.

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Primitive.h"
#include "render/ObjectRender.h"
#include "sokol/SokolPipelineCache.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    struct BoxScene {
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};
        std::vector<std::unique_ptr<Primitive>> boxes;

        BoxScene(){
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 4.0f, 12.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            sun.setType(LightType::DIRECTIONAL);
            sun.setShadows(false);
            Engine::setScene(&scene);
        }
        ~BoxScene(){
            Engine::removeScene(&scene);
        }

        void add(int count){
            for (int i = 0; i < count; i++){
                boxes.push_back(std::make_unique<Primitive>(&scene));
                boxes.back()->createBox(0.2f, 0.2f, 0.2f);
                boxes.back()->setPosition((float)(boxes.size() % 20) * 0.3f - 3.0f, 0.0f, -(float)(boxes.size() / 20) * 0.3f);
            }
        }

        bool settle(){
            Scene* current = &scene;
            const bool loaded = GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); });
            GpuEnvironment::drawFrames(3);
            return loaded;
        }
    };

}

TEST(PipelineDescPoolGpu, EqualMeshesShareOneBase){
    JASPER_REQUIRE_GPU();
    BoxScene boxes;
    boxes.add(1);
    ASSERT_TRUE(boxes.settle());
    const size_t one = SokolPipelineCache::internedBaseCount();
    EXPECT_GT(one, 0u);

    boxes.add(199);
    ASSERT_TRUE(boxes.settle());
    EXPECT_EQ(SokolPipelineCache::internedBaseCount(), one) << "199 more equal boxes hold no description of their own";
}

TEST(PipelineDescPoolGpu, NoLoadDescriptionAfterDraw){
    JASPER_REQUIRE_GPU();
    {
        BoxScene boxes;
        boxes.add(20);
        ASSERT_TRUE(boxes.settle());
        EXPECT_EQ(ObjectRender::getLoadingDescCount(), 0) << "every load let go of its working description";
    }
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(ObjectRender::getLoadingDescCount(), 0);
}
