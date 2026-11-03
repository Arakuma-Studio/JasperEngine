// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The morph inputs of a mesh with fewer targets than its shader declares
// (includes/morphtarget.glsl declares them by whether normals and tangents are morphed, not by
// the mesh's target count): every one the colour, depth (shadows, depth pre-pass) and G-buffer
// (SSR) pipelines read has a buffer, the mesh's target or zeros
// (RenderSystem::bindUnfedMorphInputs). Without one, OpenGL reads zero but Vulkan reads what
// earlier draws left there: VatEndToEndTest.MorphTargetAutoVerticesMatchesItsSource's
// one-target source drew nothing on an NVIDIA GPU after the tests before it. The backend counts
// a load leaving an input of its shader unfed (ObjectRender::getUnfedVertexInputLoads), so this
// fails on any backend. A target the mesh doesn't have moves nothing at any weight, the one it
// has moves it as far as its weight says, and so does each of the 8 / 4 / 2 targets the shader
// takes (by whether normals and tangents are morphed).

#include "TestGoldenCommon.h"

#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Model.h"
#include "render/ObjectRender.h"
#include "subsystem/RenderSystem.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const int screenWidth = 640;
    const int screenHeight = 360;
    const float pixelsPerUnit = 50.0f;

    float toScreenX(float x){ return screenWidth * 0.5f + x * pixelsPerUnit; }
    int toScreenRow(float y){ return int(std::lround(screenHeight * 0.5f - y * pixelsPerUnit)); }

    // what the mesh's one target morphs
    enum class TargetKind { Positions, PositionsNormals, PositionsNormalsTangents };

    const char* kindName(TargetKind kind){
        switch (kind){
        case TargetKind::Positions: return "positions";
        case TargetKind::PositionsNormals: return "positions and normals";
        case TargetKind::PositionsNormalsTangents: return "positions, normals and tangents";
        }
        return "";
    }

    std::string base64(const std::vector<uint8_t>& bytes){
        static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        size_t i = 0;
        for (; i + 2 < bytes.size(); i += 3){
            const uint32_t v = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8) | bytes[i + 2];
            out += table[(v >> 18) & 63]; out += table[(v >> 12) & 63]; out += table[(v >> 6) & 63]; out += table[v & 63];
        }
        if (i + 1 == bytes.size()){
            const uint32_t v = uint32_t(bytes[i]) << 16;
            out += table[(v >> 18) & 63]; out += table[(v >> 12) & 63]; out += "==";
        }else if (i + 2 == bytes.size()){
            const uint32_t v = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8);
            out += table[(v >> 18) & 63]; out += table[(v >> 12) & 63]; out += table[(v >> 6) & 63]; out += '=';
        }
        return out;
    }

    // A 2 x 2 quad facing +z around the origin with `targets` morph targets, target t moving it
    // t + 2 units along +x (and, by the kind, normal and tangent deltas of zero), written as
    // <name>/mesh.gltf in a temporary directory; its absolute path
    std::string writeMorphQuad(TargetKind kind, const std::string& name, int targets = 1){
        const std::vector<float> positions = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
        const std::vector<float> normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
        const std::vector<float> tangents = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
        const std::vector<float> zeros(12, 0.0f);
        const std::vector<uint16_t> indices = {0, 1, 2, 0, 2, 3};

        std::vector<uint8_t> data;
        std::ostringstream views;
        std::ostringstream accessors;
        int count = 0;
        auto add = [&](const void* bytes, size_t size, int target, int componentType, int elements, const char* type, const std::string& extra = ""){
            while (data.size() % 4)
                data.push_back(0);
            const size_t offset = data.size();
            data.insert(data.end(), (const uint8_t*)bytes, (const uint8_t*)bytes + size);
            views << (count ? "," : "") << "{\"buffer\":0,\"byteOffset\":" << offset << ",\"byteLength\":" << size << ",\"target\":" << target << "}";
            accessors << (count ? "," : "") << "{\"bufferView\":" << count << ",\"componentType\":" << componentType << ",\"count\":" << elements
                      << ",\"type\":\"" << type << "\"" << extra << "}";
            return count++;
        };
        const int position = add(positions.data(), positions.size() * 4, 34962, 5126, 4, "VEC3", ",\"min\":[-1,-1,0],\"max\":[1,1,0]");
        const int normal = add(normals.data(), normals.size() * 4, 34962, 5126, 4, "VEC3");
        const int tangent = add(tangents.data(), tangents.size() * 4, 34962, 5126, 4, "VEC4");
        std::ostringstream target;
        std::ostringstream weights;
        for (int t = 0; t < targets; t++){
            const float x = float(t + 2);
            const std::vector<float> moved = {x, 0, 0, x, 0, 0, x, 0, 0, x, 0, 0};
            std::ostringstream bounds;
            bounds << ",\"min\":[" << x << ",0,0],\"max\":[" << x << ",0,0]";
            target << (t ? "},{" : "") << "\"POSITION\":" << add(moved.data(), moved.size() * 4, 34962, 5126, 4, "VEC3", bounds.str());
            if (kind != TargetKind::Positions)
                target << ",\"NORMAL\":" << add(zeros.data(), zeros.size() * 4, 34962, 5126, 4, "VEC3");
            if (kind == TargetKind::PositionsNormalsTangents)
                target << ",\"TANGENT\":" << add(zeros.data(), zeros.size() * 4, 34962, 5126, 4, "VEC3");
            weights << (t ? "," : "") << "0";
        }
        const int index = add(indices.data(), indices.size() * 2, 34963, 5123, int(indices.size()), "SCALAR");

        std::ostringstream json;
        json << "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0,\"name\":\"" << name << "\"}],"
             << "\"meshes\":[{\"weights\":[" << weights.str() << "],\"primitives\":[{\"attributes\":{\"POSITION\":" << position << ",\"NORMAL\":" << normal
             << ",\"TANGENT\":" << tangent << "},\"targets\":[{" << target.str() << "}],\"indices\":" << index << ",\"material\":0}]}],"
             << "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.9,0.4,0.2,1],\"metallicFactor\":0,\"roughnessFactor\":1}}],"
             << "\"buffers\":[{\"byteLength\":" << data.size() << ",\"uri\":\"data:application/octet-stream;base64," << base64(data) << "\"}],"
             << "\"bufferViews\":[" << views.str() << "],\"accessors\":[" << accessors.str() << "]}";
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("jasper-morph-inputs-" + std::to_string(::getpid())) / name;
        std::filesystem::create_directories(dir);
        const std::filesystem::path path = dir / "mesh.gltf";
        std::ofstream(path) << json.str();
        return path.string();
    }

    // what else draws the mesh besides its colour pipeline
    enum class Passes { Colour, ShadowsAndPrepass, Ssr };

    const char* passesName(Passes passes){
        switch (passes){
        case Passes::Colour: return "colour";
        case Passes::ShadowsAndPrepass: return "shadows and depth pre-pass";
        case Passes::Ssr: return "SSR G-buffer";
        }
        return "";
    }

    // an orthographic camera looking down -z at 50 pixels per unit, a directional light
    struct MorphScene {
        SceneFixture fixture;
        Camera camera;

        explicit MorphScene(Passes passes): camera(&fixture.getScene()){
            camera.setOrtho(-6.4f, 6.4f, -3.6f, 3.6f, 0.1f, 100.0f);
            camera.setPosition(0.0f, 0.0f, 10.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            scene().setGlobalIllumination(0.35f);
            Light& light = fixture.add<Light>();
            light.setType(LightType::DIRECTIONAL);
            light.setDirection(-0.3f, -0.4f, -1.0f);
            light.setIntensity(2.0f);
            light.setShadows(passes == Passes::ShadowsAndPrepass);
            scene().setDepthPrepassEnabled(passes == Passes::ShadowsAndPrepass);
            scene().setSSREnabled(passes == Passes::Ssr);
        }

        Scene& scene(){ return fixture.getScene(); }

        // frames until every mesh is loaded with its current variant, then a few more
        bool settle(){
            Scene* current = &scene();
            int extra = 0;
            return GpuEnvironment::runFrames([current, &extra](){
                if (!current->getSystem<RenderSystem>()->isAllLoaded())
                    return false;
                auto meshes = current->getComponentArray<MeshComponent>();
                for (size_t i = 0; i < meshes->size(); i++){
                    const MeshComponent& mesh = meshes->getComponentFromIndex(i);
                    if (mesh.numSubmeshes > 0 && (!mesh.loaded || mesh.needReload || mesh.needVariantReload))
                        return false;
                }
                return ++extra > 2;
            }, 600);
        }

        Screen capture(){
            EXPECT_TRUE(settle());
            Screen screen;
            EXPECT_TRUE(screen.read());
            return screen;
        }
    };

    InkBox rowInk(const Screen& screen, float worldY){
        const int row = toScreenRow(worldY);
        return screen.inkOnBackground({0, row, screenWidth, row + 1}, 24);
    }

    class MorphTargetInputsTest : public ::testing::Test {
    protected:
        void SetUp() override{
            JASPER_REQUIRE_GPU();
            GpuEnvironment::setView(screenWidth, screenHeight, screenWidth, screenHeight, Scaling::FITWIDTH);
        }
    };

}

TEST_F(MorphTargetInputsTest, OneTargetMeshFeedsEveryInputItsPipelinesDeclare){
    for (Passes passes : {Passes::Colour, Passes::ShadowsAndPrepass, Passes::Ssr}){
        for (TargetKind kind : {TargetKind::Positions, TargetKind::PositionsNormals, TargetKind::PositionsNormalsTangents}){
            SCOPED_TRACE(std::string(kindName(kind)) + " morphed, " + passesName(passes));
            const std::string path = writeMorphQuad(kind, std::string("feed_") + std::to_string(int(kind)) + "_" + std::to_string(int(passes)));
            MorphScene s(passes);
            const int unfedBefore = ObjectRender::getUnfedVertexInputLoads();
            Model& model = s.fixture.add<Model>();
            ASSERT_TRUE(model.loadModel(path));
            const Screen rest = s.capture();

            const MeshComponent& mesh = model.getComponent<MeshComponent>();
            ASSERT_EQ(mesh.numSubmeshes, 1u);
            EXPECT_TRUE(mesh.submeshes[0].hasMorphTarget);
            EXPECT_EQ(mesh.submeshes[0].hasMorphNormal, kind != TargetKind::Positions);
            EXPECT_EQ(mesh.submeshes[0].hasMorphTangent, kind == TargetKind::PositionsNormalsTangents);
            if (passes == Passes::ShadowsAndPrepass) {
                EXPECT_TRUE(mesh.submeshes[0].depthShader) << "the depth pipelines are made";
            }
            if (passes == Passes::Ssr) {
                EXPECT_TRUE(mesh.submeshes[0].gbufferShader) << "the G-buffer pipeline is made";
            }
            EXPECT_EQ(ObjectRender::getUnfedVertexInputLoads(), unfedBefore)
                << "every morph input the shaders declare has a buffer (the mesh has one target)";

            // at rest: the quad over x [-1, 1]
            const InkBox atRest = rowInk(rest, 0.0f);
            ASSERT_FALSE(atRest.empty()) << "the mesh draws";
            EXPECT_NEAR(atRest.x0, toScreenX(-1.0f), 1.5f);
            EXPECT_NEAR(atRest.x1, toScreenX(1.0f), 1.5f);

            // its target at full weight: 2 units right
            model.setMorphWeight(0, 1.0f);
            const InkBox moved = rowInk(s.capture(), 0.0f);
            ASSERT_FALSE(moved.empty());
            EXPECT_NEAR(moved.x0, toScreenX(1.0f), 1.5f);
            EXPECT_NEAR(moved.x1, toScreenX(3.0f), 1.5f);
            EXPECT_EQ(ObjectRender::getUnfedVertexInputLoads(), unfedBefore);
        }
    }
}

TEST_F(MorphTargetInputsTest, TargetsTheMeshDoesNotHaveMoveNothingAtAnyWeight){
    for (TargetKind kind : {TargetKind::Positions, TargetKind::PositionsNormals}){
        SCOPED_TRACE(std::string(kindName(kind)) + " morphed");
        MorphScene s(Passes::Colour);
        Model& model = s.fixture.add<Model>();
        ASSERT_TRUE(model.loadModel(writeMorphQuad(kind, std::string("unused_") + std::to_string(int(kind)))));
        const Screen rest = s.capture();
        const InkBox atRest = rowInk(rest, 0.0f);
        ASSERT_FALSE(atRest.empty());

        // every other weight the shader reads at 1: the inputs behind them are zeros
        for (int target = 1; target < MAX_MORPHTARGETS; target++)
            model.setMorphWeight(target, 1.0f);
        const Screen weighted = s.capture();
        size_t differing = 0;
        for (size_t p = 0; p + 3 < rest.rgba.size(); p += 4){
            if (rest.rgba[p] != weighted.rgba[p] || rest.rgba[p + 1] != weighted.rgba[p + 1] || rest.rgba[p + 2] != weighted.rgba[p + 2])
                differing++;
        }
        EXPECT_EQ(differing, 0u) << "weights of targets the mesh doesn't have change no pixel";

        // and the one it has still moves it, by half at half weight
        model.setMorphWeight(0, 0.5f);
        const InkBox half = rowInk(s.capture(), 0.0f);
        EXPECT_NEAR(half.x0, toScreenX(0.0f), 1.5f);
        EXPECT_NEAR(half.x1, toScreenX(2.0f), 1.5f);
    }
}

// Every target a mesh can have moves it: of a mesh morphing positions only, the third and
// fourth too (includes/morphtarget.glsl declared them only when exactly one of normals and
// tangents is morphed, so they moved nothing on the GPU while CpuSkin, which bakes a Vertex
// Animation from the source, applied them); of one morphing normals, its four
TEST_F(MorphTargetInputsTest, EveryTargetTheShaderTakesMovesTheMesh){
    struct Case { TargetKind kind; int targets; };
    for (const Case& c : {Case{TargetKind::Positions, MAX_MORPHTARGETS}, Case{TargetKind::PositionsNormals, 4}, Case{TargetKind::PositionsNormalsTangents, 2}}){
        SCOPED_TRACE(std::string(kindName(c.kind)) + " morphed");
        MorphScene s(Passes::ShadowsAndPrepass);
        const int unfedBefore = ObjectRender::getUnfedVertexInputLoads();
        Model& model = s.fixture.add<Model>();
        ASSERT_TRUE(model.loadModel(writeMorphQuad(c.kind, std::string("every_") + std::to_string(int(c.kind)), c.targets)));
        ASSERT_FALSE(rowInk(s.capture(), 0.0f).empty());
        EXPECT_EQ(ObjectRender::getUnfedVertexInputLoads(), unfedBefore);
        for (int t = 0; t < c.targets; t++){
            SCOPED_TRACE("target " + std::to_string(t));
            for (int w = 0; w < c.targets; w++)
                model.setMorphWeight(w, w == t ? 0.5f : 0.0f);
            // target t moves the quad (t + 2) / 2 units at half weight (the view spans x -6.4 to 6.4)
            const float shift = float(t + 2) * 0.5f;
            const InkBox moved = rowInk(s.capture(), 0.0f);
            ASSERT_FALSE(moved.empty());
            EXPECT_NEAR(moved.x0, toScreenX(shift - 1.0f), 1.5f);
            EXPECT_NEAR(moved.x1, toScreenX(shift + 1.0f), 1.5f);
        }
    }
}
