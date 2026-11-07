// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T1 DC9, R43, R45 (AC35): an edited terrain map goes up through a stream image, and what is drawn
// after an edit is what a fresh load of the edited map draws. glcore and Vulkan.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "object/Camera.h"
#include "object/Terrain.h"
#include "object/TerrainEdit.h"
#include "pool/TexturePool.h"
#include "render/TextureRender.h"
#include "subsystem/RenderSystem.h"
#include "terrain/TerrainOps.h"
#include "texture/Texture.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr int EDIT_GPU_MAP = 65;

    std::vector<uint8_t> editGpuTarget(){
        Framebuffer& target = GpuEnvironment::target();
        std::vector<uint8_t> rgba;
        readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
        return rgba;
    }

    double editGpuDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
        if (a.size() != b.size() || a.empty())
            return 255.0;
        double sum = 0.0;
        for (size_t i = 0; i < a.size(); i += 4){
            int largest = 0;
            for (int c = 0; c < 3; c++)
                largest = std::max(largest, std::abs((int)a[i + c] - (int)b[i + c]));
            sum += largest;
        }
        return sum / (double)(a.size() / 4);
    }

    // a terrain under a camera looking down at it, its height map from these 16-bit texels
    struct EditGpuStage {
        Scene scene;
        Camera camera{&scene};
        std::shared_ptr<Terrain> terrain;

        EditGpuStage(const std::string& id, const std::vector<uint16_t>& heights){
            scene.setBackgroundColor(0.1f, 0.1f, 0.12f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 5.0f, 4.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            Engine::setScene(&scene);
            terrain = std::make_shared<Terrain>(&scene);
            terrain->setSize(8.0f);
            terrain->setMaxHeight(3.0f);
            TerrainComponent& component = terrain->getComponent<TerrainComponent>();
            const size_t bytes = heights.size() * 2;
            void* raw = std::malloc(bytes);
            std::memcpy(raw, heights.data(), bytes);
            TextureData data(EDIT_GPU_MAP, EDIT_GPU_MAP, (unsigned int)bytes, ColorFormat::RED16, 1, raw);
            data.setDataOwned(false);
            component.heightMap.setData(id, data);
            component.heightMap.getData().setDataOwned(true);
            component.heightMap.setReleaseDataAfterLoad(false);
        }

        ~EditGpuStage(){
            Engine::removeScene(&scene);
        }

        bool settle(){
            Scene* current = &scene;
            const bool loaded = GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); }, 600);
            GpuEnvironment::drawFrames(4);
            return loaded;
        }
    };

}

// R43, DC9: the stream image of a 16-bit map reads back what was uploaded (R16, or floats where
// 16-bit texels do not filter)
TEST(TerrainEditGpu, StreamMapReadsBack){
    JASPER_REQUIRE_GPU();
    TextureRender render;
    ASSERT_TRUE(render.createDynamicMapTexture("t1-stream-map", 8, 8, ColorFormat::RED16));
    ASSERT_TRUE(render.isDynamicMap());
    std::vector<uint16_t> heights(64);
    for (size_t i = 0; i < heights.size(); i++) heights[i] = (uint16_t)(i * 1000);
    std::vector<uint8_t> upload;
    if (render.getUploadFormat() == ColorFormat::RED16){
        upload.resize(heights.size() * 2);
        std::memcpy(upload.data(), heights.data(), upload.size());
    }else{
        const TerrainUploadFormat format = render.getUploadFormat() == ColorFormat::R32F ? TerrainUploadFormat::Float32 : TerrainUploadFormat::Half;
        upload.resize(heights.size() * (format == TerrainUploadFormat::Float32 ? 4 : 2));
        convertTerrainHeightRect(heights.data(), 8, terrain::TexelRect{0, 0, 8, 8}, format, upload.data());
    }
    ASSERT_TRUE(render.updateTexture(upload.data(), upload.size()));
    GpuEnvironment::drawFrames(1);
    std::vector<uint8_t> back;
    ASSERT_TRUE(readTextureRaw(render, 8, 8, render.getUploadFormat(), back));
    EXPECT_EQ(back, upload);
    EXPECT_FALSE(render.updateTexture(upload.data(), upload.size()) && render.updateTexture(upload.data(), upload.size()))
        << "a second update in one frame waits for the next";
    render.destroyTexture();
    EXPECT_NO_ENGINE_ERRORS();
}

// AC35 (R43, DC9): drawn after an edit equals the same map loaded fresh
TEST(TerrainEditGpu, UploadMatchesFreshLoad){
    JASPER_REQUIRE_GPU();
    const std::vector<uint16_t> flat((size_t)EDIT_GPU_MAP * EDIT_GPU_MAP, 16000);
    std::vector<uint16_t> edited;
    std::vector<uint8_t> afterEdit, before;
    {
        EditGpuStage stage("t1-gpu-edited", flat);
        ASSERT_TRUE(stage.settle());
        before = editGpuTarget();
        TerrainComponent& component = stage.terrain->getComponent<TerrainComponent>();
        TerrainComponentStore store(&stage.scene, stage.terrain->getEntity(), component);
        terrain::Op raise;
        raise.type = terrain::OpType::Raise;
        raise.delta = 1.5f;
        raise.shape.type = terrain::ShapeType::Circle;
        raise.shape.radius = 2.5f;
        ASSERT_TRUE(terrain::applyOps(store, std::vector<terrain::Op>{raise}).ok);
        ASSERT_TRUE(stage.settle());
        TextureRender* render = component.heightMap.getRender();
        ASSERT_NE(render, nullptr);
        EXPECT_TRUE(render->isDynamicMap()) << "the edit went up through a stream image";
        afterEdit = editGpuTarget();
        const uint16_t* pixels = (const uint16_t*)component.heightMap.getData().getData();
        edited.assign(pixels, pixels + flat.size());
    }
    ASSERT_GT(editGpuDifference(afterEdit, before), 1.0) << "the edit shows";
    EditGpuStage fresh("t1-gpu-fresh", edited);
    ASSERT_TRUE(fresh.settle());
    EXPECT_LT(editGpuDifference(editGpuTarget(), afterEdit), 1.0) << "what the edit drew is what a fresh load of the map draws";
    EXPECT_NO_ENGINE_ERRORS();
}
