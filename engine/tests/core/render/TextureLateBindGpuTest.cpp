// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A material binds its images when its mesh loads (and again when told to). An image it names
// that has no GPU texture yet and nothing loading it (no pixels in the pool, no generator, a
// generator whose model went) is bound as a stand-in: white, a flat normal. When the image can
// be made later (its pixels pooled, a generator registered: what a model's load or its
// TextureDataPool::setGenerator does), the mesh must bind it then, in every slot, rather than
// draw the stand-in for good (BF15: test_khronos_models' white label plates). Each case draws a
// plane with the image ready from the start (the reference), then a plane whose image comes
// after its first draw: that one must end up as the reference. glcore and Vulkan.

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Primitive.h"
#include "pool/TextureDataPool.h"
#include "pool/TexturePool.h"
#include "subsystem/RenderSystem.h"
#include "texture/Material.h"
#include "texture/MaterialPbr.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    enum class Slot { BaseColor, MetallicRoughness, Occlusion, Emissive, Normal, SheenMap };

    struct LateCase {
        const char* name;
        Slot slot;
        uint8_t rgba[4];
        // whether the slot changes the image enough to compare it (the others: that the
        // renderer made the image's GPU texture)
        bool visible;
    };

    std::ostream& operator << (std::ostream& os, const LateCase& c){
        return os << c.name;
    }

    std::vector<uint8_t> readTarget(){
        Framebuffer& target = GpuEnvironment::target();
        std::vector<uint8_t> rgba;
        readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
        return rgba;
    }

    // mean over the pixels of the largest channel difference (0..255)
    double meanDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
        if (a.size() != b.size() || a.empty())
            return 255.0;
        double sum = 0.0;
        const size_t pixels = a.size() / 4;
        for (size_t i = 0; i < pixels; i++){
            int largest = 0;
            for (int c = 0; c < 3; c++)
                largest = std::max(largest, std::abs((int)a[i * 4 + c] - (int)b[i * 4 + c]));
            sum += largest;
        }
        return sum / (double)pixels;
    }

    // a 4x4 image of one colour, made by a generator as a glTF image's pixels are made again
    void registerImage(const std::string& id, const uint8_t rgba[4]){
        const uint8_t r = rgba[0], g = rgba[1], b = rgba[2], a = rgba[3];
        TextureDataPool::setGenerator(id, [r, g, b, a]() -> TextureData {
            const int size = 4 * 4 * 4;
            unsigned char* pixels = static_cast<unsigned char*>(std::malloc(size));
            if (!pixels)
                return TextureData();
            for (int i = 0; i < size; i += 4){
                pixels[i] = r;
                pixels[i + 1] = g;
                pixels[i + 2] = b;
                pixels[i + 3] = a;
            }
            // not owned: the pool's copy owns the pixels
            return TextureData(4, 4, (unsigned int)size, ColorFormat::RGBA, 4, pixels);
        });
    }

    // the material of the case with `id` in its slot
    Material caseMaterial(Slot slot, const std::string& id){
        Material material;
        Texture texture;
        texture.setId(id);
        material.baseColorFactor = Vector4(0.9f, 0.9f, 0.9f, 1.0f);
        material.metallicFactor = 0.0f;
        material.roughnessFactor = 0.6f;
        switch (slot){
        case Slot::BaseColor:
            material.baseColorTexture = texture;
            break;
        case Slot::MetallicRoughness:
            // a white stand-in is fully metallic: the diffuse goes
            material.metallicFactor = 1.0f;
            material.roughnessFactor = 1.0f;
            material.metallicRoughnessTexture = texture;
            break;
        case Slot::Occlusion:
            material.occlusionTexture = texture;
            break;
        case Slot::Emissive:
            material.baseColorFactor = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
            material.emissiveFactor = Vector3(1.0f, 1.0f, 1.0f);
            material.emissiveTexture = texture;
            break;
        case Slot::Normal:
            material.normalTexture = texture;
            break;
        case Slot::SheenMap: {
            // (a variant samples UVs for a core map only: a white base colour image, there from
            // the start, gives it them)
            static const uint8_t white[4] = {255, 255, 255, 255};
            registerImage("bf15-white", white);
            Texture base;
            base.setId("bf15-white");
            material.baseColorTexture = base;
            MaterialPbrExtensions& pbr = material.pbr.edit();
            pbr.sheen.enabled = true;
            pbr.sheen.color = Vector3(1.0f, 1.0f, 1.0f);
            pbr.sheen.roughness = 0.5f;
            pbr.sheen.map = texture;
            break;
        }
        }
        return material;
    }

    class TextureLateBindGpu : public ::testing::TestWithParam<LateCase> {
    protected:
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};

        void SetUp() override{
            scene.setBackgroundColor(0.1f, 0.1f, 0.12f, 1.0f);
            // ambient light: what the occlusion map darkens
            scene.setGlobalIllumination(0.8f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 2.2f, 2.6f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(-0.5f, -1.0f, -0.3f);
            sun.setIntensity(1.2f);
            sun.setShadows(false);
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
        }

        bool render(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }

        // a plane with the material, drawn: the image of the frame
        std::vector<uint8_t> drawPlane(Primitive& plane, const Material& material){
            plane.createPlane(3.0f, 3.0f);
            plane.setMaterial(material);
            EXPECT_TRUE(render()) << "the scene never finished loading";
            GpuEnvironment::drawFrames(3);
            return readTarget();
        }
    };

}

TEST_P(TextureLateBindGpu, AnImageReadyAfterTheMaterialWasBoundIsBound){
    JASPER_REQUIRE_GPU();
    const LateCase& c = GetParam();
    static int serial = 0;
    serial++;
    const std::string readyId = std::string("bf15-ready|") + c.name + "|" + std::to_string(serial);
    const std::string lateId = std::string("bf15-late|") + c.name + "|" + std::to_string(serial);

    // the reference: the image there before the mesh loads
    registerImage(readyId, c.rgba);
    std::vector<uint8_t> reference;
    {
        Primitive plane(&scene);
        reference = drawPlane(plane, caseMaterial(c.slot, readyId));
    }

    // the image comes after the mesh was drawn with a stand-in for it
    Primitive plane(&scene);
    const std::vector<uint8_t> standIn = drawPlane(plane, caseMaterial(c.slot, lateId));
    registerImage(lateId, c.rgba);

    Texture late;
    late.setId(lateId);
    const std::string renderId = late.getRenderId();
    Scene* current = &scene;
    GpuEnvironment::runFrames([current, renderId](){
        return current->getSystem<RenderSystem>()->isAllLoaded() && TexturePool::get(renderId) != nullptr;
    }, 120);
    GpuEnvironment::drawFrames(3);
    const std::vector<uint8_t> after = readTarget();

    EXPECT_NE(TexturePool::get(renderId), nullptr) << c.name << ": the mesh never made the GPU texture of its image";
    if (c.visible){
        ASSERT_GT(meanDifference(standIn, reference), 4.0) << c.name << ": the stand-in draws as the image (the case shows nothing)";
        EXPECT_LT(meanDifference(after, reference), 1.0) << c.name << ": the mesh still draws the stand-in for its image";
    }
    EXPECT_NO_ENGINE_ERRORS();
}

INSTANTIATE_TEST_SUITE_P(Slots, TextureLateBindGpu, ::testing::Values(
    LateCase{"BaseColor", Slot::BaseColor, {230, 40, 30, 255}, true},
    LateCase{"MetallicRoughness", Slot::MetallicRoughness, {0, 140, 0, 255}, true},
    LateCase{"Occlusion", Slot::Occlusion, {0, 0, 0, 255}, true},
    LateCase{"Emissive", Slot::Emissive, {20, 220, 40, 255}, true},
    // (a primitive has no tangents: its normal map changes nothing to compare)
    LateCase{"Normal", Slot::Normal, {250, 128, 150, 255}, false},
    LateCase{"SheenMap", Slot::SheenMap, {0, 0, 0, 255}, false}),
    [](const ::testing::TestParamInfo<LateCase>& info){ return std::string(info.param.name); });
