// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The Outline component drawn (docs/specs/S94-rendering-anim-gaps.md F22, F23, F-PF1, AC22, AC23):
// QuickOutline's five modes in both width units around a red cube whose right part a blue wall
// covers, read at three probes (the band beside the visible part, the covered part, the band
// beside the covered part) and as goldens; a disabled or absent Outline adds no draw and an
// enabled one two per submesh; an Outline on a model root outlines its skinned child where the
// skin moved it and its nested static child. glcore and Vulkan (lavapipe).

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/OutlineComponent.h"
#include "object/Camera.h"
#include "object/Model.h"
#include "object/Outline.h"
#include "object/Primitive.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"
#include "TempDir.h"

#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr int OUTLINE_VIEW = 128;

    // a probe's colour: the dominant channel, or black
    char classify(const std::vector<uint8_t>& rgba, int column, int row){
        const size_t i = ((size_t)row * OUTLINE_VIEW + (size_t)column) * 4;
        const int r = rgba[i], g = rgba[i + 1], b = rgba[i + 2];
        if (r < 40 && g < 40 && b < 40) return 'K';
        if (r > 150 && g < 100 && b < 100) return 'R';
        if (g > 150 && r < 100 && b < 100) return 'G';
        if (b > 150 && r < 100 && g < 100) return 'B';
        return '?';
    }

    std::vector<uint8_t> readTarget(){
        Framebuffer& target = GpuEnvironment::target();
        std::vector<uint8_t> rgba;
        readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
        return rgba;
    }

    class OutlineGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override{
            GpuEnvironment::setView(OUTLINE_VIEW, OUTLINE_VIEW, OUTLINE_VIEW, OUTLINE_VIEW, Scaling::STRETCH);
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            // one world unit is one pixel
            camera.setType(CameraType::CAMERA_ORTHO);
            camera.setOrtho(-64.0f, 64.0f, -64.0f, 64.0f, 1.0f, 300.0f);
            camera.setPosition(0.0f, 0.0f, 100.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
            GpuEnvironment::resetView();
        }

        bool render(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }
    };

    // the red cube x -30..10, y -20..20; the blue wall in front of it from x 0 on
    struct OccludedCube {
        Primitive cube;
        Primitive wall;
        explicit OccludedCube(Scene* scene): cube(scene), wall(scene){
            cube.createBox(40.0f, 40.0f, 40.0f);
            cube.setPosition(-10.0f, 0.0f, 0.0f);
            cube.setColor(1.0f, 0.0f, 0.0f);
            wall.createBox(80.0f, 100.0f, 4.0f);
            wall.setPosition(40.0f, 0.0f, 40.0f);
            wall.setColor(0.0f, 0.0f, 1.0f);
        }
    };

    // the probes (column, row; rows top first): 1.5 px left of the visible part, inside the
    // covered part, 1.5 px above the covered part; and inside the visible part
    constexpr int VISIBLE_BAND[2] = {32, 64};
    constexpr int COVERED[2] = {69, 64};
    constexpr int COVERED_BAND[2] = {69, 42};
    constexpr int VISIBLE_BODY[2] = {44, 64};

    struct ModeCase {
        OutlineMode mode;
        const char* name;
        char visibleBand, covered, coveredBand;
    };

    const ModeCase MODE_CASES[] = {
        {OutlineMode::OUTLINE_ALL, "all", 'G', 'B', 'G'},
        {OutlineMode::OUTLINE_VISIBLE, "visible", 'G', 'B', 'B'},
        {OutlineMode::OUTLINE_HIDDEN, "hidden", 'K', 'B', 'G'},
        {OutlineMode::OUTLINE_AND_SILHOUETTE, "and_silhouette", 'G', 'G', 'G'},
        {OutlineMode::SILHOUETTE_ONLY, "silhouette_only", 'K', 'G', 'B'},
    };

}

TEST_F(OutlineGpu, Modes){
    JASPER_REQUIRE_GPU();
    const LogCapture log;
    OccludedCube objects(&scene);
    Outline outline(&scene, objects.cube.getEntity());
    outline.setColor(Vector4(0.0f, 1.0f, 0.0f, 1.0f));
    outline.setWidth(6.0f);
    ASSERT_TRUE(render());
    for (OutlineWidthUnits units : {OutlineWidthUnits::PIXELS, OutlineWidthUnits::WORLD}){
        outline.setWidthUnits(units);
        const std::string unitsName = units == OutlineWidthUnits::PIXELS ? "pixels" : "world";
        for (const ModeCase& mode : MODE_CASES){
            SCOPED_TRACE(std::string(mode.name) + " " + unitsName);
            outline.setMode(mode.mode);
            ASSERT_TRUE(render());
            const std::vector<uint8_t> rgba = readTarget();
            EXPECT_EQ(classify(rgba, VISIBLE_BAND[0], VISIBLE_BAND[1]), mode.visibleBand);
            EXPECT_EQ(classify(rgba, COVERED[0], COVERED[1]), mode.covered);
            EXPECT_EQ(classify(rgba, COVERED_BAND[0], COVERED_BAND[1]), mode.coveredBand);
            EXPECT_EQ(classify(rgba, VISIBLE_BODY[0], VISIBLE_BODY[1]), 'R');
            EXPECT_GOLDEN("outline_" + std::string(mode.name) + "_" + unitsName);
        }
    }
    EXPECT_TRUE(log.errors().empty()) << log.describeErrors();
}

// F-PF1: an absent or disabled Outline draws nothing more; an enabled one draws a mask and a fill
// per submesh, and the renders go with the component
TEST_F(OutlineGpu, DisabledAddsNothing){
    JASPER_REQUIRE_GPU();
    OccludedCube objects(&scene);
    ASSERT_TRUE(render());
    const uint32_t absent = Engine::getFrameStats().drawCalls;
    const std::vector<uint8_t> absentImage = readTarget();

    scene.addComponent<OutlineComponent>(objects.cube.getEntity(), {});
    scene.getComponent<OutlineComponent>(objects.cube.getEntity()).enabled = false;
    ASSERT_TRUE(render());
    EXPECT_EQ(Engine::getFrameStats().drawCalls, absent);
    EXPECT_TRUE(readTarget() == absentImage) << "the image is the one without an Outline";

    scene.getComponent<OutlineComponent>(objects.cube.getEntity()).enabled = true;
    ASSERT_TRUE(render());
    EXPECT_EQ(Engine::getFrameStats().drawCalls, absent + 2);
    EXPECT_TRUE(readTarget() != absentImage) << "the outline shows";

    // removed: the same draws and image as before it came
    scene.removeComponent<OutlineComponent>(objects.cube.getEntity());
    ASSERT_TRUE(render());
    EXPECT_EQ(Engine::getFrameStats().drawCalls, absent);
    EXPECT_TRUE(readTarget() == absentImage) << "the image is the one without an Outline";
}

namespace {

    // A model whose root holds no mesh: RootNode/Inner/Body, a box skinned to the joint Armature
    // (moved 1.5 to the right), and RootNode/Inner/Prop, a static box up and to the left
    std::string writeOutlinedModel(TempDir& dir){
        std::string bin;
        std::string views, accessors;
        int count = 0;
        const auto add = [&](const void* data, size_t size, int target, const std::string& type, int componentType, size_t elements, const std::string& extra){
            while (bin.size() % 4) bin.push_back('\0');
            const size_t offset = bin.size();
            bin.append(static_cast<const char*>(data), size);
            views += std::string(count ? "," : "") + R"({"buffer": 0, "byteOffset": )" + std::to_string(offset) + R"(, "byteLength": )" + std::to_string(size) +
                     (target ? R"(, "target": )" + std::to_string(target) : std::string()) + "}";
            accessors += std::string(count ? "," : "") + R"({"bufferView": )" + std::to_string(count) + R"(, "componentType": )" + std::to_string(componentType) +
                         R"(, "count": )" + std::to_string(elements) + R"(, "type": ")" + type + "\"" + extra + "}";
            return count++;
        };
        const std::array<float, 24> positions = {-1, -1, -0.5f, 1, -1, -0.5f, 1, 1, -0.5f, -1, 1, -0.5f,
                                                 -1, -1, 0.5f, 1, -1, 0.5f, 1, 1, 0.5f, -1, 1, 0.5f};
        const std::array<uint16_t, 36> indices = {4, 5, 6, 4, 6, 7, 1, 0, 3, 1, 3, 2, 5, 1, 2, 5, 2, 6,
                                                  0, 4, 7, 0, 7, 3, 7, 6, 2, 7, 2, 3, 0, 1, 5, 0, 5, 4};
        std::array<uint16_t, 32> jointIds = {};
        std::array<float, 32> weights = {};
        for (size_t v = 0; v < 8; v++)
            weights[v * 4] = 1.0f;
        const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const int position = add(positions.data(), sizeof(positions), 34962, "VEC3", 5126, 8, R"(, "min": [-1, -1, -0.5], "max": [1, 1, 0.5])");
        const int index = add(indices.data(), sizeof(indices), 34963, "SCALAR", 5123, 36, "");
        const int joint = add(jointIds.data(), sizeof(jointIds), 34962, "VEC4", 5123, 8, "");
        const int weight = add(weights.data(), sizeof(weights), 34962, "VEC4", 5126, 8, "");
        const int matrices = add(identity, sizeof(identity), 0, "MAT4", 5126, 1, "");

        std::ofstream(dir.file("outlined.bin"), std::ios::binary) << bin;
        std::ofstream(dir.file("outlined.gltf")) << R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
"nodes": [
  {"name": "RootNode", "children": [1]},
  {"name": "Inner", "children": [2, 3, 4]},
  {"name": "Armature", "translation": [1.5, 0, 0]},
  {"name": "Body", "mesh": 0, "skin": 0},
  {"name": "Prop", "mesh": 1, "translation": [-2, 2, 0]}],
"skins": [{"joints": [2], "inverseBindMatrices": )" << matrices << R"(}],
"meshes": [
  {"primitives": [{"attributes": {"POSITION": )" << position << R"(, "JOINTS_0": )" << joint << R"(, "WEIGHTS_0": )" << weight <<
            R"(}, "indices": )" << index << R"(}]},
  {"primitives": [{"attributes": {"POSITION": )" << position << R"(}, "indices": )" << index << R"(}]}],
"buffers": [{"uri": "outlined.bin", "byteLength": )" << bin.size() << R"(}], "bufferViews": [)" << views << R"(], "accessors": [)" << accessors << "]}";
        return dir.file("outlined.gltf").generic_string();
    }

}

// F23: an Outline on a model root outlines the meshes below it: the skinned one where its joint
// moved it (not at its bind pose), and the static one nested beside it
TEST_F(OutlineGpu, ModelRootOutlinesSkinnedChild){
    JASPER_REQUIRE_GPU();
    TempDir dir;
    const LogCapture log;
    // 16 pixels per unit
    camera.setOrtho(-4.0f, 4.0f, -4.0f, 4.0f, 1.0f, 300.0f);
    camera.setPosition(0.0f, 0.0f, 10.0f);
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(writeOutlinedModel(dir)));
    ASSERT_TRUE(render());
    ASSERT_EQ(scene.findComponent<MeshComponent>(model.getEntity()) == nullptr ||
              scene.getComponent<MeshComponent>(model.getEntity()).numSubmeshes == 0, true)
        << "the root draws no mesh of its own";

    // the band 1.5 px right of the skinned box (x 0.5..2.5), the band 1.5 px below the static
    // box (x -3..-1, y 1..3), and left of where the skinned box's bind pose would be
    constexpr int SKINNED_BAND[2] = {105, 64};
    constexpr int STATIC_BAND[2] = {32, 49};
    constexpr int BIND_POSE_BAND[2] = {46, 64};
    const std::vector<uint8_t> before = readTarget();
    EXPECT_EQ(classify(before, SKINNED_BAND[0], SKINNED_BAND[1]), 'K');
    EXPECT_EQ(classify(before, STATIC_BAND[0], STATIC_BAND[1]), 'K');

    Outline outline(&scene, model.getEntity());
    outline.setColor(Vector4(0.0f, 1.0f, 0.0f, 1.0f));
    outline.setWidth(4.0f);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> after = readTarget();
    EXPECT_EQ(classify(after, SKINNED_BAND[0], SKINNED_BAND[1]), 'G');
    EXPECT_EQ(classify(after, STATIC_BAND[0], STATIC_BAND[1]), 'G');
    EXPECT_EQ(classify(after, BIND_POSE_BAND[0], BIND_POSE_BAND[1]), 'K');
    EXPECT_GOLDEN("outline_model_root");

    // Include Children off: the root has no mesh, nothing is outlined
    outline.setIncludeChildren(false);
    ASSERT_TRUE(render());
    EXPECT_TRUE(readTarget() == before) << "nothing outlined";
    EXPECT_TRUE(log.errors().empty()) << log.describeErrors();
}
