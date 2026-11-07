// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S41 AC11 (R14): a 2D scene without a Pixel Perfect Camera and without an integer Scaling draws
// exactly what it drew before S41: the same passes, the same number of draws, the same shader
// variants, and no more allocations on the main thread in a steady frame. In this process: the
// frame without the component is the baseline (printed), and a disabled component, or one added
// and removed, leaves that frame pixel for pixel.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/PixelPerfectCameraComponent.h"
#include "component/UIComponent.h"
#include "object/Camera.h"
#include "object/PixelPerfectCamera.h"
#include "object/Sprite.h"
#include "object/Tilemap.h"
#include "object/ui/Image.h"
#include "object/ui/Text.h"
#include "pool/ShaderPool.h"
#include "render/RenderStats.h"
#include "render/SystemRender.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // a 16 x 16 checker in two colours; the pixels outlive every texture made of them
    const std::vector<uint8_t>& zeroCostCheckerPixels(){
        static const std::vector<uint8_t> pixels = [](){
            std::vector<uint8_t> rgba(16 * 16 * 4);
            for (int y = 0; y < 16; y++){
                for (int x = 0; x < 16; x++){
                    const bool on = ((x + y) & 1) == 0;
                    uint8_t* p = &rgba[(y * 16 + x) * 4];
                    p[0] = on ? 255 : 0;
                    p[1] = on ? 0 : 255;
                    p[2] = 0;
                    p[3] = 255;
                }
            }
            return rgba;
        }();
        return pixels;
    }

    TextureData zeroCostChecker(){
        return TextureData(16, 16, 16 * 16 * 4, ColorFormat::RGBA, 4, (void*)zeroCostCheckerPixels().data());
    }

    // what the frame is made of: its passes in order, its draws, the variants its objects drew
    // with (sorted), and the main thread's allocations in a steady frame
    struct ZeroCostFrame {
        std::vector<std::string> passes;
        uint32_t draws = 0;
        std::vector<std::string> shaders;
        uint64_t allocations = 0;
        uint64_t image = 0;     // FNV-1a of the target's pixels
    };

    std::string serializeZeroCost(const ZeroCostFrame& record){
        std::ostringstream out;
        out << "draws " << record.draws << "\n";
        out << "allocations " << record.allocations << "\n";
        out << "image " << record.image << "\n";
        for (const std::string& pass : record.passes)
            out << "pass " << pass << "\n";
        for (const std::string& shader : record.shaders)
            out << "shader " << shader << "\n";
        return out.str();
    }


    // the GPU environment's view as the suite found it: the suites after it draw at their own sizes
    struct ZeroCostGpuView {
        bool saved = false;
        int screenWidth = 0, screenHeight = 0, canvasWidth = 0, canvasHeight = 0;
        Scaling scaling = Scaling::FITWIDTH;
        void save(){
            if (!GpuEnvironment::ensure()) return;
            screenWidth = (int)GpuEnvironment::target().getWidth();
            screenHeight = (int)GpuEnvironment::target().getHeight();
            canvasWidth = Engine::getPreferredCanvasWidth();
            canvasHeight = Engine::getPreferredCanvasHeight();
            scaling = Engine::getScalingMode();
            saved = true;
        }
        void restore() const{
            if (saved) GpuEnvironment::setView(screenWidth, screenHeight, canvasWidth, canvasHeight, scaling);
        }
    };

    class PixelPerfectOffGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        ZeroCostGpuView view;

        void SetUp() override{
            view.save();
            GpuEnvironment::setView(320, 180, 320, 180, Scaling::LETTERBOX);
            scene.setBackgroundColor(0.1f, 0.1f, 0.2f, 1.0f);
            // a 2D game's camera: UI type, spanning the canvas
            camera.setType(CameraType::CAMERA_UI);
            camera.activate();
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
            view.restore();
        }
    };

}

TEST_F(PixelPerfectOffGpu, ZeroCost){
    JASPER_REQUIRE_GPU();

    // sprites (one odd-sized with a CENTER pivot at a half pixel), a tilemap, a nine-slice
    // Image and a Text: the drawables S41's snapping touches when it is on
    Sprite hero(&scene);
    hero.setTexture("pixelart|zero-cost|checker", zeroCostChecker());
    hero.setSize(15, 16);
    hero.setPivotPreset(PivotPreset::CENTER);
    hero.setPosition(40.5f, 60.25f, 0.0f);

    Sprite turned(&scene);
    turned.setTexture("pixelart|zero-cost|checker", zeroCostChecker());
    turned.setSize(16, 16);
    turned.setPosition(120.0f, 40.0f, 0.0f);
    turned.setRotation(0.0f, 0.0f, 37.0f);

    Tilemap floor(&scene);
    floor.setTexture("pixelart|zero-cost|checker", zeroCostChecker());
    floor.addRect(0, "tile", Rect(0, 0, 16, 16));
    for (int i = 0; i < 4; i++)
        floor.addTile(0, Vector2(16.0f * i, 0.0f), 16, 16);
    floor.setPosition(0.0f, 120.0f, 0.0f);

    Image panel(&scene);
    panel.setTexture("pixelart|zero-cost|checker", zeroCostChecker());
    panel.setSize(48, 32);
    panel.setPatchMargin(4);
    panel.setPosition(200.0f, 20.0f, 0.0f);

    Text label(&scene);
    label.setText("S41");
    label.setPosition(200.0f, 120.0f, 0.0f);

    Scene* current = &scene;
    auto settle = [current](){
        return GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); });
    };
    ASSERT_TRUE(settle()) << "the scene never finished loading";

    // the frame as drawn now: passes, draws, the variants its objects drew with, the main
    // thread's allocations in a steady frame, and its pixels
    auto recordFrame = [this](ZeroCostFrame& record){
        // a reader asks for the passes' details: the frames after record them
        SystemRender::getPassList(SystemRender::FRAME_STATS_FRAME, false);
        GpuEnvironment::drawFrames(2);
        // the least of a few frames (a frame now and then grows a container the others reuse)
        uint64_t least = UINT64_MAX;
        for (int i = 0; i < 5; i++){
            testsupport::ThreadAllocationCount count;
            Engine::systemDraw();
            least = std::min(least, count.count());
        }
        record.allocations = testsupport::countsAllocations() ? least : 0;
        record.draws = Engine::getFrameStats().drawCalls;
        record.passes.clear();
        const PassList passes = SystemRender::getPassList(SystemRender::FRAME_STATS_FRAME, false);
        for (const PassRecord& pass : passes.passes)
            record.passes.push_back(pass.label);

        Framebuffer& target = GpuEnvironment::target();
        std::vector<uint8_t> rgba;
        if (readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba)){
            uint64_t hash = 1469598103934665603ull;
            for (uint8_t byte : rgba){
                hash ^= byte;
                hash *= 1099511628211ull;
            }
            record.image = hash;
        }

        std::set<std::string> shaders;
        auto meshes = scene.getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); i++){
            const MeshComponent& mesh = meshes->getComponentFromIndex(i);
            for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
                shaders.insert(ShaderPool::getShaderStr(ShaderType::MESH, mesh.submeshes[s].shaderProperties));
                if (mesh.submeshes[s].depthShaderProperties != 0)
                    shaders.insert(ShaderPool::getShaderStr(ShaderType::DEPTH, mesh.submeshes[s].depthShaderProperties));
            }
        }
        auto uis = scene.getComponentArray<UIComponent>();
        for (size_t i = 0; i < uis->size(); i++)
            shaders.insert(ShaderPool::getShaderStr(ShaderType::UI, uis->getComponentFromIndex(i).shaderProperties));
        record.shaders.assign(shaders.begin(), shaders.end());
    };

    // the baseline: this scene without a Pixel Perfect Camera, in this process
    ZeroCostFrame baseline;
    recordFrame(baseline);
    std::printf("[S41] zero cost, the baseline frame without the component:\n%s", serializeZeroCost(baseline).c_str());
    ASSERT_FALSE(baseline.passes.empty()) << "no pass was recorded for the frame";
    ASSERT_GT(baseline.draws, 0u);


    // the same frame after the component came and went, and with a disabled one: nothing of the
    // feature stays behind (passes, draws, variants, allocations, pixels; same process, same driver)
    auto expectBaseline = [&](const ZeroCostFrame& frame, const char* what){
        SCOPED_TRACE(what);
        EXPECT_EQ(frame.passes, baseline.passes) << serializeZeroCost(frame);
        EXPECT_EQ(frame.draws, baseline.draws);
        EXPECT_EQ(frame.shaders, baseline.shaders) << serializeZeroCost(frame);
        EXPECT_EQ(frame.image, baseline.image) << "other pixels than without the component";
        if (testsupport::countsAllocations()){
            EXPECT_LE(frame.allocations, baseline.allocations);
        }
    };
    {
        PixelPerfectCamera added(&scene, camera.getEntity());
        added.setReferenceResolution(Vector2(320.0f, 180.0f));
        ASSERT_TRUE(settle());
        ZeroCostFrame drawing;
        recordFrame(drawing);
        EXPECT_NE(drawing.shaders, baseline.shaders) << "the component drew with its 'Pxs' variants (the check sees them)";
    }
    scene.getComponent<PixelPerfectCameraComponent>(camera.getEntity()).enabled = false;
    ASSERT_TRUE(settle());
    ZeroCostFrame disabled;
    recordFrame(disabled);
    expectBaseline(disabled, "a disabled component");
    scene.removeComponent<PixelPerfectCameraComponent>(camera.getEntity());
    ASSERT_TRUE(settle());
    ZeroCostFrame removed;
    recordFrame(removed);
    expectBaseline(removed, "the component removed");

    EXPECT_NO_ENGINE_ERRORS();
}
