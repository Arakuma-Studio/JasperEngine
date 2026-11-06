// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A mesh variant declares only the vertex inputs the mesh feeds
// (docs/superpowers/specs/2026-11-04-render-correctness-design.md 6.2). The built-in Lit's lit
// and 2D light paths asked for normals whatever the mesh had, so a mesh without normals (a glTF
// without NORMAL, a mesh built by a script) left a_normal unfed: zero on GL (a NaN normal, black),
// undefined on Vulkan, invalid on Metal and D3D11 (SokolObject::checkVertexInputs counts it). It
// now shades with flat normals, as glTF asks, turned toward the eye: the derivatives they come
// from change handedness with the backend's framebuffer y. The 2D lights take tangents only for a
// normal map.
// And a mesh with two UV sets and no normal map samples each texture at its own set (the GL
// upload of a flattened block whose tail no variant reads, sokol's PATCHES.md). glcore and Vulkan.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Light2D.h"
#include "object/Mesh.h"
#include "pool/TextureDataPool.h"
#include "render/ObjectRender.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "texture/Material.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    struct InputsQuad {
        bool normals = false;
        bool tangents = false;
        bool uv1 = false;
        bool uv2 = false;
    };

    // a 2x2 quad on y = 0 facing +y, with only the attributes asked for: UV set 1 at (0, 0) and
    // set 2 at (1, 1) on every vertex, so a texture's set decides which texel draws
    void fillInputsQuad(Scene& scene, Entity entity, const InputsQuad& in){
        MeshComponent& mesh = scene.getComponent<MeshComponent>(entity);
        mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
        mesh.numSubmeshes = 1;
        mesh.buffer.clear();
        mesh.buffer.addAttribute(AttributeType::POSITION, 3);
        if (in.uv1) mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
        if (in.uv2) mesh.buffer.addAttribute(AttributeType::TEXCOORD2, 2);
        if (in.normals) mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
        if (in.tangents) mesh.buffer.addAttribute(AttributeType::TANGENT, 4);
        const std::array<Vector3, 4> corners = {Vector3(-1, 0, -1), Vector3(-1, 0, 1), Vector3(1, 0, 1), Vector3(1, 0, -1)};
        for (const Vector3& corner : corners){
            mesh.buffer.addVector3(AttributeType::POSITION, corner);
            if (in.uv1) mesh.buffer.addVector2(AttributeType::TEXCOORD1, Vector2(0.1f, 0.5f));
            if (in.uv2) mesh.buffer.addVector2(AttributeType::TEXCOORD2, Vector2(0.9f, 0.5f));
            if (in.normals) mesh.buffer.addVector3(AttributeType::NORMAL, Vector3(0, 1, 0));
            if (in.tangents) mesh.buffer.addVector4(AttributeType::TANGENT, Vector4(1, 0, 0, 1));
        }
        std::vector<uint16_t> indices = {0, 1, 2, 0, 2, 3};
        mesh.indices.clear();
        mesh.indices.setValues(0, mesh.indices.getAttribute(AttributeType::INDEX), indices.size(), (char*)indices.data(), sizeof(uint16_t));
        scene.getSystem<MeshSystem>()->calculateMeshAABB(mesh);
    }

    // an 8x8 image of two halves (left, right)
    Texture inputsSplitTexture(const std::string& id, std::array<uint8_t, 4> left, std::array<uint8_t, 4> right){
        TextureDataPool::setGenerator(id, [left, right]() -> TextureData {
            const int size = 8 * 8 * 4;
            unsigned char* pixels = static_cast<unsigned char*>(std::malloc(size));
            if (!pixels)
                return TextureData();
            for (int y = 0; y < 8; y++){
                for (int x = 0; x < 8; x++){
                    const std::array<uint8_t, 4>& texel = x < 4 ? left : right;
                    std::copy(texel.begin(), texel.end(), pixels + (y * 8 + x) * 4);
                }
            }
            return TextureData(8, 8, (unsigned int)size, ColorFormat::RGBA, 4, pixels);
        });
        Texture texture;
        texture.setId(id);
        return texture;
    }

    std::array<int, 3> inputsCenterPixel(){
        Framebuffer& target = GpuEnvironment::target();
        std::vector<uint8_t> rgba;
        readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
        const int w = (int)target.getWidth(), h = (int)target.getHeight();
        if (rgba.size() < (size_t)(w * h * 4))
            return {0, 0, 0};
        const size_t i = ((size_t)(h / 2) * w + w / 2) * 4;
        return {rgba[i], rgba[i + 1], rgba[i + 2]};
    }

    enum class InputsLighting { Pixel, Vertex, Mixed, Lights2D };

    struct InputsCase {
        const char* name;
        InputsLighting lighting;
        bool tangents;
    };

    std::ostream& operator << (std::ostream& os, const InputsCase& c){
        return os << c.name;
    }

    class MeshInputsGpu : public ::testing::TestWithParam<InputsCase> {};

    // the scene of every case: a black background, a camera above the quad
    struct InputsQuadScene {
        Scene scene;
        Camera camera{&scene};
        Mesh quad{&scene};

        InputsQuadScene(){
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            // no ambient: only the light's side of the face may draw lit
            scene.setGlobalIllumination(0.0f);
            scene.setAmbientLight2D(0.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 3.0f, 0.01f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            Engine::setScene(&scene);
        }

        ~InputsQuadScene(){
            Engine::removeScene(&scene);
        }

        bool settle(){
            Scene* current = &scene;
            const bool loaded = GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
            GpuEnvironment::drawFrames(3);
            return loaded;
        }

        // the camera above the quad (its front, +y) or below it (its back)
        void viewFrom(bool above){
            camera.setPosition(0.0f, above ? 3.0f : -3.0f, 0.01f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            GpuEnvironment::drawFrames(3);
        }

        const Submesh& submesh() const{
            return scene.getComponent<MeshComponent>(quad.getEntity()).submeshes[0];
        }
    };

}

TEST_P(MeshInputsGpu, ANormallessMeshFeedsEveryInput){
    JASPER_REQUIRE_GPU();
    const InputsCase& c = GetParam();
    const int unfed = ObjectRender::getUnfedVertexInputLoads();
    {
        InputsQuadScene s;
        Light sun(&s.scene);
        Light2D light2d(&s.scene);
        if (c.lighting == InputsLighting::Lights2D){
            sun.setEnabled(false);
            light2d.setPosition(0.0f, 0.5f, 0.0f);
            light2d.setRange(10.0f);
            light2d.setIntensity(2.0f);
        }else{
            light2d.setEnabled(false);
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(0.0f, -1.0f, 0.0f);
            sun.setIntensity(3.0f);
            sun.setShadows(false);
        }

        InputsQuad in;
        in.tangents = c.tangents;
        fillInputsQuad(s.scene, s.quad.getEntity(), in);
        Material material;
        material.baseColorFactor = Vector4(0.8f, 0.8f, 0.8f, 1.0f);
        material.roughnessFactor = 1.0f;
        material.metallicFactor = 0.0f;
        if (c.lighting == InputsLighting::Vertex) material.lighting = MaterialLighting::VERTEX;
        if (c.lighting == InputsLighting::Mixed) material.lighting = MaterialLighting::MIXED;
        s.quad.setMaterial(material);
        s.quad.setFaceCulling(false);

        ASSERT_TRUE(s.settle()) << c.name << ": the scene never finished loading";
        const uint64_t properties = s.submesh().shaderProperties;
        EXPECT_EQ(properties & (uint64_t(1) << 6), 0u) << c.name << ": a mesh without normals asks for 'Nor'";
        EXPECT_EQ(properties & (uint64_t(1) << 8), 0u) << c.name << ": tangents without normals ('Tan')";
        std::array<int, 3> center = inputsCenterPixel();
        EXPECT_GT(center[0] + center[1] + center[2], 30) << c.name << ": the lit quad draws black";

        if (c.lighting != InputsLighting::Lights2D){
            // the sun behind the face leaves it dark
            sun.setDirection(0.0f, 1.0f, 0.0f);
            s.viewFrom(true);
            center = inputsCenterPixel();
            EXPECT_LT(center[0] + center[1] + center[2], 10) << c.name << ": the face lit by a light behind it";
        }
        if (c.lighting == InputsLighting::Pixel || c.lighting == InputsLighting::Mixed){
            // the back of a double-sided face, from below: lit by a sun below, dark under one
            // above (the vertex lights take the mesh's +y, the face's front, for both sides)
            s.viewFrom(false);
            center = inputsCenterPixel();
            EXPECT_GT(center[0] + center[1] + center[2], 30) << c.name << ": the back face draws black under a light on its side";
            sun.setDirection(0.0f, -1.0f, 0.0f);
            s.viewFrom(false);
            center = inputsCenterPixel();
            EXPECT_LT(center[0] + center[1] + center[2], 10) << c.name << ": the back face lit by a light on the other side";
        }
    }
    EXPECT_EQ(ObjectRender::getUnfedVertexInputLoads(), unfed) << c.name << ": a pipeline left a vertex input without its buffer";
    EXPECT_NO_ENGINE_ERRORS();
}

INSTANTIATE_TEST_SUITE_P(InputsLighting, MeshInputsGpu, ::testing::Values(
    InputsCase{"Pixel", InputsLighting::Pixel, false},
    InputsCase{"PixelWithTangents", InputsLighting::Pixel, true},
    InputsCase{"Vertex", InputsLighting::Vertex, false},
    InputsCase{"Mixed", InputsLighting::Mixed, true},
    InputsCase{"Lights2D", InputsLighting::Lights2D, false},
    InputsCase{"Lights2DWithTangents", InputsLighting::Lights2D, true}),
    [](const ::testing::TestParamInfo<InputsCase>& info){ return std::string(info.param.name); });

// the 2D lights read tangents only for a normal map: a mesh with tangents and no map takes none
TEST(MeshInputsGpuSingle, TwoDLightsTakeTangentsOnlyWithANormalMap){
    JASPER_REQUIRE_GPU();
    InputsQuadScene s;
    Light2D light2d(&s.scene);
    light2d.setPosition(0.0f, 0.5f, 0.0f);
    light2d.setRange(10.0f);

    InputsQuad in;
    in.normals = true;
    in.tangents = true;
    in.uv1 = true;
    fillInputsQuad(s.scene, s.quad.getEntity(), in);
    Material material;
    material.lighting = MaterialLighting::NONE;
    s.quad.setMaterial(material);
    ASSERT_TRUE(s.settle());
    EXPECT_EQ(s.submesh().shaderProperties & (uint64_t(1) << 22), uint64_t(1) << 22) << "the case is not on the 2D light path ('L2d')";
    EXPECT_EQ(s.submesh().shaderProperties & (uint64_t(1) << 8), 0u) << "'Tan' without a normal map";

    material.normalTexture = inputsSplitTexture("rc-flat-normal", {128, 128, 255, 255}, {128, 128, 255, 255});
    s.quad.setMaterial(material);
    ASSERT_TRUE(s.settle());
    EXPECT_NE(s.submesh().shaderProperties & (uint64_t(1) << 7), 0u) << "no 'Nmp' with the normal map";
    EXPECT_NE(s.submesh().shaderProperties & (uint64_t(1) << 8), 0u) << "no 'Tan' with the normal map";
    EXPECT_NO_ENGINE_ERRORS();
}

// the base colour on UV set 2, the metallic-roughness map on set 1, no normal map: nothing reads
// the block's second vec4 (set1), which a driver may leave out of the flattened array
TEST(MeshInputsGpuSingle, TwoUvSetsWithoutANormalMapSampleTheirSets){
    JASPER_REQUIRE_GPU();
    InputsQuadScene s;
    s.scene.setGlobalIllumination(1.0f);
    Light sun(&s.scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(0.0f, -1.0f, 0.0f);
    sun.setIntensity(2.0f);
    sun.setShadows(false);

    InputsQuad in;
    in.normals = true;
    in.uv1 = true;
    in.uv2 = true;
    fillInputsQuad(s.scene, s.quad.getEntity(), in);
    Material material;
    material.metallicFactor = 0.0f;
    material.baseColorTexture = inputsSplitTexture("rc-uv-red-green", {255, 0, 0, 255}, {0, 255, 0, 255});
    material.baseColorTexCoord = 1;
    material.metallicRoughnessTexture = inputsSplitTexture("rc-uv-rough", {255, 255, 0, 255}, {255, 255, 0, 255});
    material.metallicRoughnessTexCoord = 0;
    s.quad.setMaterial(material);
    ASSERT_TRUE(s.settle());
    EXPECT_NE(s.submesh().shaderProperties & (uint64_t(1) << 2), 0u) << "the variant has no second UV set ('Uv2')";
    EXPECT_EQ(s.submesh().shaderProperties & (uint64_t(1) << 7), 0u) << "the case has a normal map";
    const std::array<int, 3> center = inputsCenterPixel();
    EXPECT_GT(center[1], center[0] + 20) << "the base colour did not sample UV set 2 (red is set 1's texel)";
    EXPECT_NO_ENGINE_ERRORS();
}
