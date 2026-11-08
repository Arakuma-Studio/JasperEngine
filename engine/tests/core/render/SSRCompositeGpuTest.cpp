// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The SSR composite swaps the environment specular a surface already has for the screen-space
// reflection instead of adding to it (docs/superpowers/specs/2026-11-04-ssr-probes-design.md,
// 10.3). The scene: a metal floor, a white unlit box standing on it that SSR reflects, a
// directional light. "The hit" is the floor pixels where the SSR debug view Reflection shows the
// box's white at full confidence. Each test compares two renders of the same frame through the
// same SSR path; Intensity 0 is SSR's composite with no reflection (every pixel returned as the
// colour pass drew it). Only engine API older than the change is used here, so these tests run
// on the commit before it too (the red controls of the fixes).

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Mesh.h"
#include "object/Primitive.h"
#include "object/environment/Fog.h"
#include "object/environment/ReflectionProbe.h"
#include "object/environment/Skybox.h"
#include "shader/ShaderUniforms.h"
#include "profiler/GpuMemory.h"
#include "render/SystemRender.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "texture/Material.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"
#include "TempDir.h"
#include "TestPaths.h"

#include "stb_image_write.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // the SSR debug views (Scene::setSSRDebugMode)
    constexpr int VIEW_OFF = 0;
    constexpr int VIEW_REFLECTION = 1;
    constexpr int VIEW_NORMAL = 2;
    constexpr int VIEW_ROUGHNESS = 3;
    constexpr int VIEW_ALBEDO = 5;
    constexpr int VIEW_ENVIRONMENT = 6;
    constexpr int VIEW_SOURCE = 7;
    constexpr int VIEW_OCCLUSION = 8;

    std::vector<uint8_t> ssrReadTarget(){
        Framebuffer& target = GpuEnvironment::target();
        std::vector<uint8_t> rgba;
        readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
        return rgba;
    }

    int ssrChannelDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, size_t pixel){
        int largest = 0;
        for (int c = 0; c < 3; c++)
            largest = std::max(largest, std::abs((int)a[pixel * 4 + c] - (int)b[pixel * 4 + c]));
        return largest;
    }

    // the largest channel difference over some pixels (all of them when the list is empty)
    int ssrMaxDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, const std::vector<size_t>& pixels = {}){
        if (a.size() != b.size() || a.empty())
            return 255;
        int largest = 0;
        if (pixels.empty()){
            for (size_t p = 0; p < a.size() / 4; p++)
                largest = std::max(largest, ssrChannelDiff(a, b, p));
        }else{
            for (size_t p : pixels)
                largest = std::max(largest, ssrChannelDiff(a, b, p));
        }
        return largest;
    }

    // a measured residual, printed so the tolerances can be checked against what a run gives
    int ssrMeasured(const char* what, int value){
        std::printf("[measured] %s: %d/255\n", what, value);
        return value;
    }

    // an image of vertical bands of the colours (RGBA, left to right), its pixels kept alive by the caller
    TextureData ssrImage(std::vector<uint8_t>& pixels, int width, int height, const std::vector<uint32_t>& colors){
        pixels.assign((size_t)width * height * 4, 255);
        for (int y = 0; y < height; y++){
            for (int x = 0; x < width; x++){
                const uint32_t c = colors[(size_t)x * colors.size() / (size_t)width];
                uint8_t* p = &pixels[((size_t)y * width + x) * 4];
                p[0] = (uint8_t)(c >> 24);
                p[1] = (uint8_t)(c >> 16);
                p[2] = (uint8_t)(c >> 8);
                p[3] = (uint8_t)c;
            }
        }
        return TextureData(width, height, (unsigned int)pixels.size(), ColorFormat::RGBA, 4, pixels.data());
    }

    // A cube face of a smooth colour field of the elevation (a function of the direction's y squared,
    // so it is continuous across the faces whatever their image orientation): the small turn of the
    // G-buffer's 8-bit normal changes a sample by a hair, while a wrong box projection, capture
    // position, mip or slot does not. `order` picks which channel rises with the elevation.
    void ssrWriteGradientFace(const std::string& path, int face, const int order[3]){
        constexpr int SIZE = 32;
        std::vector<uint8_t> pixels((size_t)SIZE * SIZE * 4, 255);
        for (int y = 0; y < SIZE; y++){
            for (int x = 0; x < SIZE; x++){
                const float sc = 2.0f * ((float)x + 0.5f) / SIZE - 1.0f;
                const float tc = 2.0f * ((float)y + 0.5f) / SIZE - 1.0f;
                Vector3 d;
                switch (face){
                    case 0:  d = Vector3(1.0f, -tc, -sc); break;  // +X
                    case 1:  d = Vector3(-1.0f, -tc, sc); break;  // -X
                    case 2:  d = Vector3(sc, 1.0f, tc); break;    // +Y
                    case 3:  d = Vector3(sc, -1.0f, -tc); break;  // -Y
                    case 4:  d = Vector3(sc, -tc, 1.0f); break;   // +Z
                    default: d = Vector3(-sc, -tc, -1.0f); break; // -Z
                }
                d.normalize();
                const float up = d.y * d.y;
                const float c[3] = {up, 0.3f + 0.4f * up, 1.0f - up};
                uint8_t* p = &pixels[((size_t)y * SIZE + x) * 4];
                for (int k = 0; k < 3; k++)
                    p[k] = (uint8_t)std::lround(c[order[k]] * 255.0f);
            }
        }
        ASSERT_TRUE(stbi_write_png(path.c_str(), SIZE, SIZE, 4, pixels.data(), SIZE * 4)) << path;
    }

    class SSRCompositeGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};
        std::unique_ptr<Primitive> floor;
        std::unique_ptr<Primitive> box;

        void SetUp() override{
            scene.setBackgroundColor(0.08f, 0.08f, 0.1f, 1.0f);
            scene.setLitQuality(LitQuality::STANDARD);
            scene.setGlobalIllumination(0.0f);
            scene.setSSREnabled(true);
            scene.setSSRMaxDistance(30.0f);
            scene.setSSRMaxSteps(128);
            scene.setSSRThickness(0.6f);
            scene.setSSRIntensity(1.0f);
            scene.setSSRBlur(0.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 2.0f, 6.0f);
            camera.setTarget(0.0f, 0.2f, -2.0f);
            camera.setFarClip(200.0f);
            camera.activate();
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
        }

        // made once the device exists (shaders then compile synchronously)
        void build(float floorRoughness = 0.05f){
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(0.3f, -1.0f, 0.4f);
            sun.setIntensity(1.0f);
            sun.setShadows(false);

            floor = std::make_unique<Primitive>(&scene);
            floor->createPlane(30.0f, 30.0f);
            Material ground = floor->getMaterial();
            ground.baseColorFactor = Vector4(0.5f, 0.5f, 0.5f, 1.0f);
            ground.metallicFactor = 1.0f;
            ground.roughnessFactor = floorRoughness;
            floor->setMaterial(ground);
            // lit by the sky and the probes where a test has them
            floor->setReceiveIBL(true);

            box = std::make_unique<Primitive>(&scene);
            box->createBox(3.0f, 2.0f, 1.0f);
            box->setPosition(0.0f, 1.0f, -4.0f);
            setBoxColor(1.0f);
        }

        void setBoxColor(float value){
            setBoxColor(Vector4(value, value, value, 1.0f));
        }
        void setBoxColor(const Vector4& color){
            Material material = box->getMaterial();
            material.lighting = MaterialLighting::NONE;
            material.baseColorFactor = color;
            box->setMaterial(material);
        }

        void setFloorMaterial(const std::function<void(Material&)>& change){
            Material material = floor->getMaterial();
            change(material);
            floor->setMaterial(material);
        }

        bool render(int extraFrames = 3){
            Scene* current = &scene;
            const bool loaded = GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
            GpuEnvironment::drawFrames(extraFrames);
            return loaded;
        }

        std::vector<uint8_t> shot(int view, float intensity = 1.0f, int extraFrames = 3){
            scene.setSSRDebugMode(view);
            scene.setSSRIntensity(intensity);
            EXPECT_TRUE(render(extraFrames));
            return ssrReadTarget();
        }

        // the floor pixels where SSR reflects the (white, or red) box at full confidence
        std::vector<size_t> hit(bool redBox = false){
            const std::vector<uint8_t> reflection = shot(VIEW_REFLECTION);
            std::vector<size_t> pixels;
            for (size_t p = 0; p < reflection.size() / 4; p++){
                const bool full = reflection[p * 4] >= 245 &&
                    (redBox || (reflection[p * 4 + 1] >= 245 && reflection[p * 4 + 2] >= 245));
                if (full)
                    pixels.push_back(p);
            }
            return pixels;
        }

        // a sky with IBL: six faces of one colour
        void addSky(std::unique_ptr<Skybox>& sky, uint32_t rgba){
            static std::vector<uint8_t> faces[6];
            sky = std::make_unique<Skybox>(&scene);
            auto face = [&](int f){ return ssrImage(faces[f], 8, 8, {rgba}); };
            sky->setTextures("ssr-composite-sky-" + std::to_string(rgba), face(0), face(1), face(2), face(3), face(4), face(5));
        }

        // the target pixel a world point is drawn at (rows top first)
        size_t pixelOf(const Vector3& world){
            const Vector4 clip = camera.getViewProjectionMatrix() * Vector4(world.x, world.y, world.z, 1.0f);
            const int width = (int)GpuEnvironment::target().getWidth();
            const int height = (int)GpuEnvironment::target().getHeight();
            const int x = std::clamp((int)std::lround((clip.x / clip.w * 0.5f + 0.5f) * width), 0, width - 1);
            const int y = std::clamp((int)std::lround((1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * height), 0, height - 1);
            return (size_t)y * width + x;
        }
    };

    // enough confident reflection to judge by
    constexpr size_t MIN_HIT_PIXELS = 200;

}

TEST_F(SSRCompositeGpu, LocalProbeIsReplacedNotAdded){
    JASPER_REQUIRE_GPU();
    build();
    // the probe captures a room of one colour, red (the background and the box SSR reflects): the
    // G-buffer's 8-bit normal turns the rebuilt reflection a little, which must not move it across
    // an edge in the capture, here the box's
    scene.setBackgroundColor(1.0f, 0.0f, 0.0f, 1.0f);
    setBoxColor(Vector4(1.0f, 0.0f, 0.0f, 1.0f));
    auto probe = std::make_unique<ReflectionProbe>(&scene);
    probe->setPosition(0.0f, 1.5f, 0.0f);
    probe->setBoxSize(30.0f, 10.0f, 30.0f);
    ASSERT_TRUE(render(40));

    const std::vector<size_t> hitWithProbe = hit(true);
    const std::vector<uint8_t> withProbe = shot(VIEW_OFF);
    // what the composite swaps there (for the message): Reflection Source and Environment Specular
    const std::vector<uint8_t> source = shot(VIEW_SOURCE);
    const std::vector<uint8_t> environment = shot(VIEW_ENVIRONMENT);
    const std::vector<uint8_t> roughness = shot(VIEW_ROUGHNESS);
    const std::vector<uint8_t> normal = shot(VIEW_NORMAL);
    // the colour pass alone (no reflection swapped), with the probe's term
    const std::vector<uint8_t> plainWithProbe = shot(VIEW_OFF, 0.0f);

    probe.reset();
    const std::vector<uint8_t> plainWithoutProbe = shot(VIEW_OFF, 0.0f, 5);
    const std::vector<uint8_t> withoutProbe = shot(VIEW_OFF, 1.0f);
    // where SSR reflects the box at full confidence in both frames (the floor's variant changes with the
    // probe, so the two frames' SSR is compared only where both found the box)
    const std::vector<size_t> hitWithoutProbe = hit(true);
    std::vector<size_t> pixels;
    std::set_intersection(hitWithProbe.begin(), hitWithProbe.end(), hitWithoutProbe.begin(), hitWithoutProbe.end(),
                          std::back_inserter(pixels));
    ASSERT_GE(pixels.size(), MIN_HIT_PIXELS);
    // where SSR is confident it is the reflection: the probe's term is swapped out, not kept
    size_t worst = pixels[0];
    for (size_t p : pixels){
        if (ssrChannelDiff(withProbe, withoutProbe, p) > ssrChannelDiff(withProbe, withoutProbe, worst))
            worst = p;
    }
    auto rgb = [](const std::vector<uint8_t>& image, size_t p){
        return std::to_string(image[p * 4]) + "," + std::to_string(image[p * 4 + 1]) + "," + std::to_string(image[p * 4 + 2]);
    };
    EXPECT_LE(ssrMeasured("probe swap", ssrChannelDiff(withProbe, withoutProbe, worst)), 3)
        << "pixel " << worst << " of " << pixels.size() << ": with the probe " << rgb(withProbe, worst) << ", without "
        << rgb(withoutProbe, worst) << "; Reflection Source " << rgb(source, worst) << ", Environment Specular "
        << rgb(environment, worst) << ", Roughness " << rgb(roughness, worst) << ", Normal " << rgb(normal, worst)
        << "; colour pass alone with the probe " << rgb(plainWithProbe, worst) << ", without " << rgb(plainWithoutProbe, worst);
    // the floor is in the probe's slot 0 at full weight
    EXPECT_GT(source[worst * 4], 240);
    EXPECT_LT(source[worst * 4 + 1], 15);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(SSRCompositeGpu, MaterialOcclusionOccludesTheSwap){
    JASPER_REQUIRE_GPU();
    std::unique_ptr<Skybox> sky;
    addSky(sky, 0x1A1A4DFFu);
    build();
    static std::vector<uint8_t> black, white;
    setFloorMaterial([](Material& m){
        m.occlusionTexture = Texture("ssr-composite-ao-black", ssrImage(black, 4, 4, {0x000000FFu}));
    });
    ASSERT_TRUE(render());

    const std::vector<size_t> pixels = hit();
    ASSERT_GE(pixels.size(), MIN_HIT_PIXELS);
    // fully occluded: the colour pass added no environment, nothing to swap or reflect
    EXPECT_LE(ssrMeasured("material occlusion", ssrMaxDiff(shot(VIEW_OFF), shot(VIEW_OFF, 0.0f), pixels)), 2);

    // unoccluded, the same floor does take the reflection (SSR is on here)
    setFloorMaterial([](Material& m){
        m.occlusionTexture = Texture("ssr-composite-ao-white", ssrImage(white, 4, 4, {0xFFFFFFFFu}));
    });
    ASSERT_TRUE(render());
    EXPECT_GE(ssrMaxDiff(shot(VIEW_OFF), shot(VIEW_OFF, 0.0f), pixels), 30);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(SSRCompositeGpu, ScreenOcclusionWeighsTheSwap){
    JASPER_REQUIRE_GPU();
    std::unique_ptr<Skybox> sky;
    addSky(sky, 0x1A1A4DFFu);
    build();
    scene.setSSAOEnabled(true);
    scene.setSSAORadius(1.0f);
    scene.setSSAOIntensity(2.0f);
    ASSERT_TRUE(render());

    // the Occlusion view (no occlusion map here: SSAO alone) is the AO buffer the colour pass read
    const std::vector<uint8_t> occlusion = shot(VIEW_OCCLUSION);
    scene.setSSAODebug(true);
    const std::vector<uint8_t> ssao = shot(VIEW_OFF, 0.0f);
    std::vector<size_t> surface;
    bool occluded = false;
    for (size_t p = 0; p < occlusion.size() / 4; p++){
        // the background is black in the debug views
        if (occlusion[p * 4] == 0 && occlusion[p * 4 + 1] == 0 && occlusion[p * 4 + 2] == 0)
            continue;
        surface.push_back(p);
        occluded = occluded || occlusion[p * 4] < 200;
    }
    ASSERT_GE(surface.size(), MIN_HIT_PIXELS);
    EXPECT_TRUE(occluded) << "SSAO darkens the floor at the box's foot";
    EXPECT_LE(ssrMeasured("screen occlusion", ssrMaxDiff(occlusion, ssao, surface)), 2);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(SSRCompositeGpu, FogAttenuatesTheSwap){
    JASPER_REQUIRE_GPU();
    build();
    ASSERT_TRUE(render());
    const std::vector<size_t> pixels = hit();
    ASSERT_GE(pixels.size(), MIN_HIT_PIXELS);

    // the floor under the hit is about 4-8 m away: almost all fog there
    Fog fog(&scene);
    fog.setType(FogType::EXPONENTIAL);
    fog.setColor(0.7f, 0.72f, 0.78f);
    fog.setDensity(0.8f);
    EXPECT_LE(ssrMeasured("distance fog", ssrMaxDiff(shot(VIEW_OFF), shot(VIEW_OFF, 0.0f), pixels)), 2);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(SSRCompositeGpu, HeightFogAttenuatesTheSwap){
    JASPER_REQUIRE_GPU();
    build();
    ASSERT_TRUE(render());
    const std::vector<size_t> pixels = hit();
    ASSERT_GE(pixels.size(), MIN_HIT_PIXELS);

    Fog fog(&scene);
    fog.setType(FogType::HEIGHT);
    fog.setColor(0.7f, 0.72f, 0.78f);
    fog.setAttenuationDistance(0.5f);
    fog.setBaseHeight(20.0f);
    fog.setMaximumHeight(40.0f);
    fog.setStartDistance(0.0f);
    fog.setMaxOpacity(1.0f);
    EXPECT_LE(ssrMeasured("height fog", ssrMaxDiff(shot(VIEW_OFF), shot(VIEW_OFF, 0.0f), pixels)), 2);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(SSRCompositeGpu, UnlitSurfacesReceiveNoSSR){
    JASPER_REQUIRE_GPU();
    build();
    ASSERT_TRUE(render());
    const std::vector<size_t> pixels = hit();
    ASSERT_GE(pixels.size(), MIN_HIT_PIXELS);

    // an unlit mirror-like floor: no specular, nothing for SSR to swap or add to
    setFloorMaterial([](Material& m){ m.lighting = MaterialLighting::NONE; });
    EXPECT_EQ(ssrMaxDiff(shot(VIEW_OFF), shot(VIEW_OFF, 0.0f)), 0);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(SSRCompositeGpu, AmbientSpecularIsReplaced){
    JASPER_REQUIRE_GPU();
    build();
    ASSERT_TRUE(render());
    const std::vector<size_t> pixels = hit();
    ASSERT_GE(pixels.size(), MIN_HIT_PIXELS);

    // no sky, no probe: Lit's environment is the scene ambient, which SSR replaces
    const std::vector<uint8_t> dark = shot(VIEW_OFF);
    scene.setGlobalIllumination(0.6f, Vector3(1.0f, 1.0f, 1.0f));
    const std::vector<uint8_t> lit = shot(VIEW_OFF);
    EXPECT_LE(ssrMeasured("ambient swap", ssrMaxDiff(dark, lit, pixels)), 3);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(SSRCompositeGpu, PixelsWithoutReflectionAreUntouched){
    JASPER_REQUIRE_GPU();
    std::unique_ptr<Skybox> sky;
    addSky(sky, 0x3366CCFFu);
    // too rough for SSR: no pixel reflects
    build(0.9f);
    ASSERT_TRUE(render());
    EXPECT_EQ(ssrMaxDiff(shot(VIEW_OFF), shot(VIEW_OFF, 0.0f)), 0);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(SSRCompositeGpu, GBufferReadsTheMaterialsUVSets){
    JASPER_REQUIRE_GPU();
    build();
    // a floor whose first UV set stays on one texel and whose second spans the floor
    floor.reset();
    Mesh ground(&scene);
    MeshComponent& mesh = ground.getComponent<MeshComponent>();
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.numSubmeshes = 1;
    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD2, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    Attribute* position = mesh.buffer.getAttribute(AttributeType::POSITION);
    Attribute* uv1 = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
    Attribute* uv2 = mesh.buffer.getAttribute(AttributeType::TEXCOORD2);
    Attribute* normal = mesh.buffer.getAttribute(AttributeType::NORMAL);
    const float corners[4][2] = {{-10.0f, -10.0f}, {10.0f, -10.0f}, {10.0f, 10.0f}, {-10.0f, 10.0f}};
    for (const auto& corner : corners){
        mesh.buffer.addVector3(position, Vector3(corner[0], 0.0f, corner[1]));
        mesh.buffer.addVector2(uv1, Vector2(0.25f, 0.5f));
        mesh.buffer.addVector2(uv2, Vector2((corner[0] + 10.0f) / 20.0f, (corner[1] + 10.0f) / 20.0f));
        mesh.buffer.addVector3(normal, Vector3(0.0f, 1.0f, 0.0f));
    }
    // front faces up
    const std::vector<uint16_t> indices = {0, 3, 2, 0, 2, 1};
    mesh.indices.clear();
    mesh.indices.setValues(0, mesh.indices.getAttribute(AttributeType::INDEX), indices.size(), (char*)&indices[0], sizeof(uint16_t));
    scene.getSystem<MeshSystem>()->calculateMeshAABB(mesh);

    // red | green on the second set, the occlusion black | white on it too
    static std::vector<uint8_t> colors, occlusion;
    Material material = ground.getMaterial();
    material.metallicFactor = 1.0f;
    material.roughnessFactor = 0.05f;
    material.baseColorTexture = Texture("ssr-composite-uv2-color", ssrImage(colors, 16, 4, {0xFF0000FFu, 0x00FF00FFu}));
    material.baseColorTexCoord = 1;
    material.occlusionTexture = Texture("ssr-composite-uv2-ao", ssrImage(occlusion, 16, 4, {0x000000FFu, 0xFFFFFFFFu}));
    material.occlusionTexCoord = 1;
    ground.setMaterial(material);
    ground.setReceiveIBL(true);
    ASSERT_TRUE(render());

    const size_t left = pixelOf(Vector3(-5.0f, 0.0f, 1.0f));
    const size_t right = pixelOf(Vector3(5.0f, 0.0f, 1.0f));
    // the albedo SSR reflects is the colour drawn: red on the left, green on the right
    const std::vector<uint8_t> albedo = shot(VIEW_ALBEDO);
    EXPECT_GT(albedo[left * 4], 200);
    EXPECT_LT(albedo[left * 4 + 1], 60);
    EXPECT_LT(albedo[right * 4], 60);
    EXPECT_GT(albedo[right * 4 + 1], 200);
    // the occlusion read on the same set: occluded on the left only
    const std::vector<uint8_t> ao = shot(VIEW_OCCLUSION);
    EXPECT_LT(ao[left * 4], 30);
    EXPECT_GT(ao[right * 4], 225);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(SSRCompositeGpu, IntensityAboveOneNeverRemovesMoreThanTheEnvironment){
    JASPER_REQUIRE_GPU();
    std::unique_ptr<Skybox> sky;
    addSky(sky, 0xE6E6E6FFu);
    build();
    ASSERT_TRUE(render());
    const std::vector<size_t> pixels = hit();
    ASSERT_GE(pixels.size(), MIN_HIT_PIXELS);

    // a dark box against a bright sky: the reflection is darker than the environment it replaces
    setBoxColor(0.05f);
    const std::vector<uint8_t> once = shot(VIEW_OFF, 1.0f);
    const std::vector<uint8_t> twice = shot(VIEW_OFF, 2.0f);
    int darker = 0;
    for (size_t p : pixels){
        for (int c = 0; c < 3; c++)
            darker = std::max(darker, (int)once[p * 4 + c] - (int)twice[p * 4 + c]);
    }
    EXPECT_LE(darker, 1);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(SSRCompositeGpu, AuthoredProbesInTwoSlotsAreReplaced){
    JASPER_REQUIRE_GPU();
    build();
    // two floor halves, each in a probe of its own authored cubemap (slots 0 and 1)
    floor.reset();
    std::unique_ptr<Primitive> halves[2];
    for (int h = 0; h < 2; h++){
        halves[h] = std::make_unique<Primitive>(&scene);
        halves[h]->createPlane(15.0f, 30.0f);
        halves[h]->setPosition(h == 0 ? -7.5f : 7.5f, 0.0f, 0.0f);
        Material ground = halves[h]->getMaterial();
        ground.baseColorFactor = Vector4(0.5f, 0.5f, 0.5f, 1.0f);
        ground.metallicFactor = 1.0f;
        ground.roughnessFactor = 0.05f;
        halves[h]->setMaterial(ground);
        halves[h]->setReceiveIBL(true);
    }
    TempDir dir;
    const int orders[2][3] = {{0, 1, 2}, {2, 0, 1}};
    std::unique_ptr<ReflectionProbe> probes[2];
    for (int h = 0; h < 2; h++){
        std::string faces[6];
        for (int f = 0; f < 6; f++){
            faces[f] = dir.file("probe" + std::to_string(h) + "_" + std::to_string(f) + ".png").generic_string();
            ssrWriteGradientFace(faces[f], f, orders[h]);
        }
        probes[h] = std::make_unique<ReflectionProbe>(&scene);
        probes[h]->setMode(ReflectionProbeMode::STATIC);
        // the capture position off the box centre, so a box projection taking the wrong one shows
        probes[h]->setPosition(h == 0 ? -5.0f : 5.0f, 2.0f, 1.0f);
        probes[h]->setBoxOffset(h == 0 ? -2.5f : 2.5f, -0.5f, -1.0f);
        probes[h]->setBoxSize(15.0f, 10.0f, 30.0f);
        probes[h]->setTextures(faces[0], faces[1], faces[2], faces[3], faces[4], faces[5]);
    }
    ASSERT_TRUE(render(30));

    const std::vector<size_t> hitWithProbes = hit();
    const std::vector<uint8_t> withProbes = shot(VIEW_OFF);
    const std::vector<uint8_t> source = shot(VIEW_SOURCE);
    for (auto& probe : probes)
        probe.reset();
    const std::vector<uint8_t> withoutProbes = shot(VIEW_OFF, 1.0f, 5);
    const std::vector<size_t> hitWithoutProbes = hit();
    std::vector<size_t> pixels;
    std::set_intersection(hitWithProbes.begin(), hitWithProbes.end(), hitWithoutProbes.begin(), hitWithoutProbes.end(),
                          std::back_inserter(pixels));
    ASSERT_GE(pixels.size(), MIN_HIT_PIXELS);

    // the reflection in both halves comes from its own slot (red slot 0, green slot 1)
    size_t slot0 = 0, slot1 = 0;
    for (size_t p : pixels){
        if (source[p * 4] > 200 && source[p * 4 + 1] < 30)
            slot0++;
        if (source[p * 4 + 1] > 200 && source[p * 4] < 30)
            slot1++;
    }
    EXPECT_GE(slot0, 20u);
    EXPECT_GE(slot1, 20u);
    // where SSR is confident, both probes' terms are swapped out
    size_t worst = pixels[0];
    for (size_t p : pixels){
        if (ssrChannelDiff(withProbes, withoutProbes, p) > ssrChannelDiff(withProbes, withoutProbes, worst))
            worst = p;
    }
    EXPECT_LE(ssrMeasured("two authored probes", ssrChannelDiff(withProbes, withoutProbes, worst)), 6)
        << "pixel " << worst << ": with the probes " << (int)withProbes[worst * 4] << "," << (int)withProbes[worst * 4 + 1]
        << "," << (int)withProbes[worst * 4 + 2] << ", without " << (int)withoutProbes[worst * 4] << ","
        << (int)withoutProbes[worst * 4 + 1] << "," << (int)withoutProbes[worst * 4 + 2] << "; source "
        << (int)source[worst * 4] << "," << (int)source[worst * 4 + 1] << "," << (int)source[worst * 4 + 2];
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(SSRCompositeGpu, StochasticTilingOcclusionOccludesTheSwap){
    JASPER_REQUIRE_GPU();
    std::unique_ptr<Skybox> sky;
    addSky(sky, 0x1A1A4DFFu);
    build();
    static std::vector<uint8_t> black;
    setFloorMaterial([](Material& m){
        m.stochasticTiling = true;
        m.occlusionTexture = Texture("ssr-composite-ao-black-stochastic", ssrImage(black, 4, 4, {0x000000FFu}));
    });
    ASSERT_TRUE(render());

    const std::vector<size_t> pixels = hit();
    ASSERT_GE(pixels.size(), MIN_HIT_PIXELS);
    // the occlusion read through Stochastic Tiling's taps, as the colour pass reads it: nothing swapped
    EXPECT_LE(ssrMeasured("stochastic occlusion", ssrMaxDiff(shot(VIEW_OFF), shot(VIEW_OFF, 0.0f), pixels)), 2);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(SSRCompositeGpu, SaturatedPixelsKeepTheirColour){
    JASPER_REQUIRE_GPU();
    std::unique_ptr<Skybox> sky;
    addSky(sky, 0xFFFFFFFFu);
    build();
    ASSERT_TRUE(render());
    const std::vector<size_t> hits = hit();
    ASSERT_GE(hits.size(), MIN_HIT_PIXELS);

    // white chrome under a white sky and the sun: the colour pass clips; a dark box to reflect
    setFloorMaterial([](Material& m){ m.baseColorFactor = Vector4(1.0f, 1.0f, 1.0f, 1.0f); });
    setBoxColor(0.05f);
    sun.setIntensity(4.0f);
    const std::vector<uint8_t> drawn = shot(VIEW_OFF, 0.0f);
    const std::vector<uint8_t> swapped = shot(VIEW_OFF, 1.0f);
    std::vector<size_t> clipped;
    for (size_t p : hits){
        if (std::max(drawn[p * 4], std::max(drawn[p * 4 + 1], drawn[p * 4 + 2])) >= 254)
            clipped.push_back(p);
    }
    ASSERT_GE(clipped.size(), 50u) << "the scene has saturated pixels under the reflection";
    // a clipped pixel lost what its colour pass added beyond white: no swap there (no dark hole)
    int darker = 0;
    for (size_t p : clipped){
        for (int c = 0; c < 3; c++)
            darker = std::max(darker, (int)drawn[p * 4 + c] - (int)swapped[p * 4 + c]);
    }
    EXPECT_LE(ssrMeasured("saturated pixels", darker), 2);
    EXPECT_NO_ENGINE_ERRORS();
}

// ---------------------------------------------------------------------------------------
// Follow-ups (2026-11-07, spec "Follow-ups"): SSR before the transparents, the composite under a
// tone curve, the G-buffer's 16-bit normal. Engine API older than the change only, as above.
// JASPER_TEST_WRITE_ACTUAL=1 keeps the images the measures were taken on (<output>/ssr/<name>.png).
// ---------------------------------------------------------------------------------------

namespace {

    void ssrWriteImage(const char* name, const std::vector<uint8_t>& rgba){
        if (!envFlag("JASPER_TEST_WRITE_ACTUAL"))
            return;
        const std::string path = testPaths().outputDir + "/ssr/" + name + ".png";
        if (!writePng(path, rgba, (int)GpuEnvironment::target().getWidth(), (int)GpuEnvironment::target().getHeight()))
            std::printf("[ssr images] could not write %s\n", path.c_str());
    }

    // |a - b| per channel, times gain, on opaque black
    std::vector<uint8_t> ssrDiffImage(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int gain){
        std::vector<uint8_t> out(a.size(), 255);
        for (size_t i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4){
            for (int c = 0; c < 3; c++)
                out[i + c] = (uint8_t)std::min(255, std::abs((int)a[i + c] - (int)b[i + c]) * gain);
        }
        return out;
    }

    // a sky face of narrow bands of the elevation (cos(BANDS * elevation), even in it, so it is
    // continuous across the faces whatever their image orientation): a reflection turned by a
    // fraction of a degree changes it by several steps
    constexpr float SSR_SKY_BANDS = 24.0f;
    TextureData ssrBandedFace(std::vector<uint8_t>& pixels, int face){
        constexpr int SIZE = 128;
        pixels.assign((size_t)SIZE * SIZE * 4, 255);
        for (int y = 0; y < SIZE; y++){
            for (int x = 0; x < SIZE; x++){
                const float sc = 2.0f * ((float)x + 0.5f) / SIZE - 1.0f;
                const float tc = 2.0f * ((float)y + 0.5f) / SIZE - 1.0f;
                Vector3 d;
                switch (face){
                    case 0:  d = Vector3(1.0f, -tc, -sc); break;
                    case 1:  d = Vector3(-1.0f, -tc, sc); break;
                    case 2:  d = Vector3(sc, 1.0f, tc); break;
                    case 3:  d = Vector3(sc, -1.0f, -tc); break;
                    case 4:  d = Vector3(sc, -tc, 1.0f); break;
                    default: d = Vector3(-sc, -tc, -1.0f); break;
                }
                d.normalize();
                const float band = 0.5f + 0.5f * std::cos(SSR_SKY_BANDS * std::asin(std::clamp(d.y, -1.0f, 1.0f)));
                uint8_t* p = &pixels[((size_t)y * SIZE + x) * 4];
                p[0] = (uint8_t)std::lround(band * 255.0f);
                p[1] = (uint8_t)std::lround((0.2f + 0.6f * band) * 255.0f);
                p[2] = (uint8_t)std::lround((1.0f - band) * 255.0f);
            }
        }
        return TextureData(SIZE, SIZE, (unsigned int)pixels.size(), ColorFormat::RGBA, 4, pixels.data());
    }

    // an unlit, alpha-blended, two-sided quad of one colour lying in the XZ plane
    void ssrTransparentQuad(Primitive& quad, float width, float depth, const Vector4& color){
        quad.createPlane(width, depth);
        Material material;
        material.name = "SSR transparent";
        material.shader = "library:unlit";
        material.blendMode = MaterialBlendMode::ALPHA;
        material.cullMode = MaterialCullMode::OFF;
        ShaderUniforms::set(material.shaderParams, "baseColor", color);
        quad.setMaterial(material);
    }

}

namespace {

    // the largest difference of two images' mean colours over the 8 x 8 blocks whose pixels all
    // pass `use` (banding shows in it, a dither's noise averages out)
    double ssrBlockMeanDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, const std::function<bool(size_t)>& use){
        const int width = (int)GpuEnvironment::target().getWidth();
        const int height = (int)GpuEnvironment::target().getHeight();
        double largest = 0.0;
        for (int by = 0; by + 8 <= height; by += 8){
            for (int bx = 0; bx + 8 <= width; bx += 8){
                double sum[3] = {0.0, 0.0, 0.0};
                bool whole = true;
                for (int y = by; y < by + 8 && whole; y++){
                    for (int x = bx; x < bx + 8; x++){
                        const size_t p = (size_t)y * width + x;
                        if (!use(p)){
                            whole = false;
                            break;
                        }
                        for (int c = 0; c < 3; c++)
                            sum[c] += (int)a[p * 4 + c] - (int)b[p * 4 + c];
                    }
                }
                if (!whole)
                    continue;
                for (int c = 0; c < 3; c++)
                    largest = std::max(largest, std::fabs(sum[c] / 64.0));
            }
        }
        return largest;
    }

    // a sky face brightest at the horizon, darker up and down (even in the elevation: continuous
    // across the faces whatever their image orientation), a long smooth gradient
    TextureData ssrGradientFace(std::vector<uint8_t>& pixels, int face){
        constexpr int SIZE = 64;
        pixels.assign((size_t)SIZE * SIZE * 4, 255);
        for (int y = 0; y < SIZE; y++){
            for (int x = 0; x < SIZE; x++){
                const float sc = 2.0f * ((float)x + 0.5f) / SIZE - 1.0f;
                const float tc = 2.0f * ((float)y + 0.5f) / SIZE - 1.0f;
                Vector3 d;
                switch (face){
                    case 0:  d = Vector3(1.0f, -tc, -sc); break;
                    case 1:  d = Vector3(-1.0f, -tc, sc); break;
                    case 2:  d = Vector3(sc, 1.0f, tc); break;
                    case 3:  d = Vector3(sc, -1.0f, -tc); break;
                    case 4:  d = Vector3(sc, -tc, 1.0f); break;
                    default: d = Vector3(-sc, -tc, -1.0f); break;
                }
                d.normalize();
                const float horizon = 1.0f - std::fabs(d.y);
                uint8_t* p = &pixels[((size_t)y * SIZE + x) * 4];
                // dim: under a strong exposure, where a step of the 8-bit store is widest on screen
                p[0] = (uint8_t)std::lround((0.03f + 0.12f * horizon) * 255.0f);
                p[1] = (uint8_t)std::lround((0.04f + 0.14f * horizon) * 255.0f);
                p[2] = (uint8_t)std::lround((0.06f + 0.18f * horizon) * 255.0f);
            }
        }
        return TextureData(SIZE, SIZE, (unsigned int)pixels.size(), ColorFormat::RGBA, 4, pixels.data());
    }

    // the root mean square of the channel differences
    double ssrRms(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
        double sum = 0.0;
        size_t count = 0;
        for (size_t i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4){
            for (int c = 0; c < 3; c++){
                const double d = (int)a[i + c] - (int)b[i + c];
                sum += d * d;
                count++;
            }
        }
        return count ? std::sqrt(sum / (double)count) : 0.0;
    }

    // the passes of the last frame
    PassList ssrPasses(){
        SystemRender::getPassList(SystemRender::FRAME_STATS_FRAME, false);
        GpuEnvironment::drawFrames(2);
        return SystemRender::getPassList(SystemRender::FRAME_STATS_FRAME, false);
    }

    bool ssrHasPass(const PassList& list, const char* label){
        for (const PassRecord& pass : list.passes){
            if (std::string(pass.label) == label)
                return true;
        }
        return false;
    }

}

// The transparent queue draws after the composite (Unity's order): SSR samples the opaque scene,
// so a glass pane in front of the box does not tint the box's reflection in the floor
TEST_F(SSRCompositeGpu, TransparentsAreNotReflected){
    JASPER_REQUIRE_GPU();
    build();
    ASSERT_TRUE(render());
    const std::vector<size_t> pixels = hit();
    ASSERT_GE(pixels.size(), MIN_HIT_PIXELS);
    const std::vector<uint8_t> plain = shot(VIEW_OFF);

    // a green pane standing right in front of the box's face, above the floor: on screen it
    // covers the box, not the floor where the box reflects
    Primitive pane(&scene);
    ssrTransparentQuad(pane, 3.0f, 2.0f, Vector4(0.0f, 1.0f, 0.0f, 0.7f));
    pane.setRotation(90.0f, 0.0f, 0.0f);
    pane.setPosition(0.0f, 1.05f, -3.4f);
    const std::vector<uint8_t> withPane = shot(VIEW_OFF);
    ssrWriteImage("transparents-not-reflected", withPane);
    EXPECT_LE(ssrMeasured("pane in the reflection", ssrMaxDiff(plain, withPane, pixels)), 2)
        << "the floor reflects the pane drawn over the box";
    EXPECT_NO_ENGINE_ERRORS();
}

// ... and a transparent over the floor covers the floor's reflection as it covers the floor: the
// swap is not added on top of what blends over the surface
TEST_F(SSRCompositeGpu, TransparentsOverTheFloorCoverItsReflection){
    JASPER_REQUIRE_GPU();
    build();
    ASSERT_TRUE(render());
    const std::vector<size_t> pixels = hit();
    ASSERT_GE(pixels.size(), MIN_HIT_PIXELS);
    const int bare = ssrMaxDiff(shot(VIEW_OFF), shot(VIEW_OFF, 0.0f), pixels);
    ASSERT_GE(bare, 30) << "the floor reflects the box";

    // nine tenths black over the whole floor in front of the box
    Primitive cover(&scene);
    ssrTransparentQuad(cover, 30.0f, 7.0f, Vector4(0.0f, 0.0f, 0.0f, 0.9f));
    cover.setPosition(0.0f, 0.02f, 0.0f);
    const std::vector<uint8_t> covered = shot(VIEW_OFF);
    const std::vector<uint8_t> coveredPlain = shot(VIEW_OFF, 0.0f);
    ssrWriteImage("transparents-cover-reflection", covered);
    // a tenth of the reflection shows through (blended in display colour), never all of it
    EXPECT_LE(ssrMeasured("reflection under the cover", ssrMaxDiff(covered, coveredPlain, pixels)), bare / 6 + 2)
        << "the bare reflection changes the floor by " << bare << "/255";
    EXPECT_NO_ENGINE_ERRORS();
}

// The same through the multisampled colour pass: its samples resolve before the composite and
// resume after it, its depth kept for the transparents
TEST_F(SSRCompositeGpu, TransparentsOverTheFloorCoverItsReflectionWithMSAA){
    JASPER_REQUIRE_GPU();
    PostProcessingSettings post = scene.getPostProcessing();
    post.msaaSamples = 4;
    scene.setPostProcessing(post);
    build();
    ASSERT_TRUE(render());
    const std::vector<size_t> pixels = hit();
    ASSERT_GE(pixels.size(), MIN_HIT_PIXELS);
    const int bare = ssrMaxDiff(shot(VIEW_OFF), shot(VIEW_OFF, 0.0f), pixels);
    ASSERT_GE(bare, 30) << "the floor reflects the box";

    Primitive cover(&scene);
    ssrTransparentQuad(cover, 30.0f, 7.0f, Vector4(0.0f, 0.0f, 0.0f, 0.9f));
    cover.setPosition(0.0f, 0.02f, 0.0f);
    // and a red pane standing behind the box, which the box hides: the opaque depth survives the
    // composite
    Primitive behind(&scene);
    ssrTransparentQuad(behind, 2.0f, 1.0f, Vector4(1.0f, 0.0f, 0.0f, 0.9f));
    behind.setRotation(90.0f, 0.0f, 0.0f);
    behind.setPosition(0.0f, 1.0f, -5.5f);
    const std::vector<uint8_t> covered = shot(VIEW_OFF);
    const std::vector<uint8_t> coveredPlain = shot(VIEW_OFF, 0.0f);
    EXPECT_LE(ssrMeasured("reflection under the cover, MSAA 4", ssrMaxDiff(covered, coveredPlain, pixels)), bare / 6 + 2)
        << "the bare reflection changes the floor by " << bare << "/255";
    const size_t boxPixel = pixelOf(Vector3(0.0f, 1.0f, -3.5f));
    EXPECT_GE(covered[boxPixel * 4 + 1], 200) << "the pane behind the box shows through it";
    EXPECT_NO_ENGINE_ERRORS();
}

// Under a tone curve the colour pass writes tone-mapped colour; the composite swaps in linear
// light and tone-maps the result as the colour pass would have, so a probe's term is still
// replaced, not added: the curve adds nothing to the swap's residual without it
TEST_F(SSRCompositeGpu, ProbeIsReplacedUnderAToneCurve){
    JASPER_REQUIRE_GPU();
    build();
    // the probe test's red room, the box a mid red the curve bends
    scene.setBackgroundColor(1.0f, 0.0f, 0.0f, 1.0f);
    setBoxColor(Vector4(0.6f, 0.0f, 0.0f, 1.0f));
    // the largest difference, where the box is reflected at full confidence in both frames, between
    // the frame with the probe and the frame without it
    auto probeSwap = [&](const char* images){
        auto probe = std::make_unique<ReflectionProbe>(&scene);
        probe->setPosition(0.0f, 1.5f, 0.0f);
        probe->setBoxSize(30.0f, 10.0f, 30.0f);
        EXPECT_TRUE(render(40));
        const std::vector<uint8_t> reflection = shot(VIEW_REFLECTION);
        const std::vector<uint8_t> withProbe = shot(VIEW_OFF);
        probe.reset();
        const std::vector<uint8_t> withoutProbe = shot(VIEW_OFF, 1.0f, 5);
        const std::vector<uint8_t> reflectionWithout = shot(VIEW_REFLECTION);
        std::vector<size_t> pixels;
        for (size_t p = 0; p < reflection.size() / 4; p++){
            if (reflection[p * 4] >= 100 && reflection[p * 4 + 1] < 20 && reflectionWithout[p * 4] >= 100 &&
                    std::abs((int)reflection[p * 4] - (int)reflectionWithout[p * 4]) <= 2)
                pixels.push_back(p);
        }
        EXPECT_GE(pixels.size(), MIN_HIT_PIXELS);
        if (images){
            ssrWriteImage((std::string(images) + "-with-probe").c_str(), withProbe);
            ssrWriteImage((std::string(images) + "-without-probe").c_str(), withoutProbe);
        }
        return pixels.empty() ? 255 : ssrMaxDiff(withProbe, withoutProbe, pixels);
    };
    const int linear = ssrMeasured("probe swap, no tone curve", probeSwap(nullptr));
    PostProcessingSettings post = scene.getPostProcessing();
    post.tonemap = TonemapMode::ACES;
    post.exposure = 1.0f;
    scene.setPostProcessing(post);
    const int curved = ssrMeasured("probe swap under ACES, exposure 1", probeSwap("tone-curve"));
    EXPECT_LE(curved, linear + 3);
    EXPECT_NO_ENGINE_ERRORS();
}

// What the composite gives where nothing reflects, under a tone curve: the colour pass's own
// image (the composite applies the curve to what it decoded)
TEST_F(SSRCompositeGpu, ToneCurveKeepsPixelsWithoutReflection){
    JASPER_REQUIRE_GPU();
    PostProcessingSettings post = scene.getPostProcessing();
    post.tonemap = TonemapMode::ACES;
    post.exposure = 1.0f;
    scene.setPostProcessing(post);
    std::unique_ptr<Skybox> sky;
    addSky(sky, 0x3366CCFFu);
    // too rough for SSR: no pixel reflects
    build(0.9f);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> on = shot(VIEW_OFF);
    scene.setSSREnabled(false);
    ASSERT_TRUE(render(5));
    const std::vector<uint8_t> off = ssrReadTarget();
    ssrWriteImage("tone-curve-no-reflection-diff-x16", ssrDiffImage(on, off, 16));
    // the linear store's dither: a step of noise per pixel, nothing on average
    EXPECT_LE(ssrMeasured("tone curve, SSR on against off", ssrMaxDiff(on, off)), 3);
    std::printf("[measured] tone curve, SSR on against off, RMS: %.2f/255\n", ssrRms(on, off));
    const double blocks = ssrBlockMeanDiff(on, off, [](size_t){ return true; });
    std::printf("[measured] tone curve, SSR on against off, 8 x 8 means: %.2f/255\n", blocks);
    EXPECT_LE(blocks, 1.5);
    EXPECT_NO_ENGINE_ERRORS();
}

// The composite rebuilds the environment specular from the G-buffer's normal: on a curved mirror
// under narrow sky bands, an 8-bit octahedral normal (up to 0.94 degrees off) turns the rebuilt
// reflection across the bands, a grain of error from pixel to pixel in the octahedral code's
// cells; the 16-bit one (0.004 degrees) leaves none against the colour pass, which shades the
// exact normal (a smooth difference stays: the rebuild's own, under 7/255 on average here)
TEST_F(SSRCompositeGpu, RebuiltEnvironmentFollowsACurvedSurface){
    JASPER_REQUIRE_GPU();
    static std::vector<uint8_t> faces[6];
    auto sky = std::make_unique<Skybox>(&scene);
    sky->setTextures("ssr-composite-banded-sky", ssrBandedFace(faces[0], 0), ssrBandedFace(faces[1], 1), ssrBandedFace(faces[2], 2),
                     ssrBandedFace(faces[3], 3), ssrBandedFace(faces[4], 4), ssrBandedFace(faces[5], 5));
    // no light: the sphere's colour is its environment specular alone
    sun.setIntensity(0.0f);
    scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
    Primitive sphere(&scene);
    sphere.createSphere(1.6f, 96, 64);
    sphere.setPosition(0.0f, 1.6f, -2.0f);
    Material chrome = sphere.getMaterial();
    chrome.baseColorFactor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    chrome.metallicFactor = 1.0f;
    chrome.roughnessFactor = 0.05f;
    sphere.setMaterial(chrome);
    sphere.setReceiveIBL(true);
    camera.setPosition(0.0f, 1.6f, 3.0f);
    camera.setTarget(0.0f, 1.6f, -2.0f);
    ASSERT_TRUE(render(30));

    const std::vector<uint8_t> drawn = shot(VIEW_OFF, 0.0f);
    const std::vector<uint8_t> rebuilt = shot(VIEW_ENVIRONMENT);
    // the sphere's inner pixels (no background within two pixels: no edge coverage in the measure)
    const int width = (int)GpuEnvironment::target().getWidth();
    const int height = (int)GpuEnvironment::target().getHeight();
    auto background = [&](int x, int y){
        const size_t p = ((size_t)y * width + x) * 4;
        return rebuilt[p] == 0 && rebuilt[p + 1] == 0 && rebuilt[p + 2] == 0;
    };
    std::vector<size_t> inner;
    for (int y = 2; y < height - 2; y++){
        for (int x = 2; x < width - 2; x++){
            bool edge = false;
            for (int dy = -2; dy <= 2 && !edge; dy++)
                for (int dx = -2; dx <= 2 && !edge; dx++)
                    edge = background(x + dx, y + dy);
            if (!edge)
                inner.push_back((size_t)y * width + x);
        }
    }
    ASSERT_GE(inner.size(), (size_t)(width * height / 10)) << "the sphere fills the middle of the view";
    // the error, and its part that changes from pixel to pixel: the error less its mean over the
    // 5 x 5 pixels around (the normal's quantisation is that; a smooth difference of the rebuild,
    // a residual of its own, is not)
    std::vector<uint8_t> isInner((size_t)width * height, 0);
    for (size_t p : inner)
        isInner[p] = 1;
    std::vector<int> errors;
    std::vector<int> grain;
    double sum = 0.0;
    for (size_t p : inner){
        errors.push_back(ssrChannelDiff(drawn, rebuilt, p));
        sum += errors.back();
        const int x = (int)(p % (size_t)width);
        const int y = (int)(p / (size_t)width);
        int largest = 0;
        for (int c = 0; c < 3; c++){
            double mean = 0.0;
            int count = 0;
            for (int dy = -2; dy <= 2; dy++){
                for (int dx = -2; dx <= 2; dx++){
                    const size_t q = (size_t)(y + dy) * width + (size_t)(x + dx);
                    if (!isInner[q])
                        continue;
                    mean += (int)drawn[q * 4 + c] - (int)rebuilt[q * 4 + c];
                    count++;
                }
            }
            const double own = (int)drawn[p * 4 + c] - (int)rebuilt[p * 4 + c];
            largest = std::max(largest, (int)std::lround(std::fabs(own - mean / count)));
        }
        grain.push_back(largest);
    }
    std::sort(errors.begin(), errors.end());
    std::sort(grain.begin(), grain.end());
    std::printf("[measured] rebuilt environment on the sphere: mean %.2f/255, 99th percentile %d/255, max %d/255\n",
                sum / (double)errors.size(), errors[errors.size() * 99 / 100], errors.back());
    ssrWriteImage("curved-mirror-drawn", drawn);
    ssrWriteImage("curved-mirror-rebuilt", rebuilt);
    ssrWriteImage("curved-mirror-error-x8", ssrDiffImage(drawn, rebuilt, 8));
    EXPECT_LE(ssrMeasured("pixel-to-pixel error of the rebuilt environment (99th percentile)", grain[grain.size() * 99 / 100]), 6);
    EXPECT_NO_ENGINE_ERRORS();
}

// The linear store is 8 bits of the shaper's code: in a dim sky brightened by the exposure, one
// step of it is several steps on screen. Undithered, a long gradient bands; dithered, the 8 x 8
// means of the image with SSR follow the image without it
TEST_F(SSRCompositeGpu, AGradientSkyDoesNotBandUnderAToneCurve){
    JASPER_REQUIRE_GPU();
    PostProcessingSettings post = scene.getPostProcessing();
    post.tonemap = TonemapMode::ACES;
    post.exposure = 3.0f;
    scene.setPostProcessing(post);
    static std::vector<uint8_t> faces[6];
    auto sky = std::make_unique<Skybox>(&scene);
    sky->setTextures("ssr-composite-gradient-sky", ssrGradientFace(faces[0], 0), ssrGradientFace(faces[1], 1), ssrGradientFace(faces[2], 2),
                     ssrGradientFace(faces[3], 3), ssrGradientFace(faces[4], 4), ssrGradientFace(faces[5], 5));
    build(0.9f);
    // looking at the sky above the horizon
    camera.setPosition(0.0f, 1.0f, 6.0f);
    camera.setTarget(0.0f, 4.0f, -6.0f);
    ASSERT_TRUE(render(10));
    const std::vector<uint8_t> on = shot(VIEW_OFF);
    scene.setSSREnabled(false);
    ASSERT_TRUE(render(5));
    const std::vector<uint8_t> off = ssrReadTarget();
    ssrWriteImage("gradient-sky-with-ssr", on);
    ssrWriteImage("gradient-sky-without-ssr", off);
    ssrWriteImage("gradient-sky-diff-x16", ssrDiffImage(on, off, 16));
    // the sky above the box
    const size_t skyEnd = (size_t)pixelOf(Vector3(0.0f, 2.2f, -4.0f)) / GpuEnvironment::target().getWidth() * GpuEnvironment::target().getWidth();
    const double blocks = ssrBlockMeanDiff(on, off, [skyEnd](size_t p){ return p < skyEnd; });
    std::printf("[measured] gradient sky, SSR on against off: 8 x 8 means %.2f/255, pixels %d/255, RMS %.2f/255\n", blocks, ssrMaxDiff(on, off),
                ssrRms(on, off));
    EXPECT_LE(blocks, 1.0);
    EXPECT_NO_ENGINE_ERRORS();
}

// Under the tone curve every pixel the colour pass drew takes it, a surface the G-buffer pass
// left out too (a mesh loaded before SSR was on, a terrain refreshing its maps): only the clear
// colour, as it was cleared, is passed through
TEST_F(SSRCompositeGpu, SurfacesOutsideTheGBufferTakeTheToneCurve){
    JASPER_REQUIRE_GPU();
    PostProcessingSettings post = scene.getPostProcessing();
    post.tonemap = TonemapMode::ACES;
    post.exposure = 1.0f;
    scene.setPostProcessing(post);
    build(0.9f);
    setBoxColor(Vector4(0.4f, 0.6f, 0.2f, 1.0f));
    ASSERT_TRUE(render());
    const std::vector<uint8_t> inGBuffer = shot(VIEW_OFF);
    // the box without its G-buffer program: the G-buffer pass skips it
    MeshComponent& mesh = box->getComponent<MeshComponent>();
    std::shared_ptr<ShaderRender> program = mesh.submeshes[0].gbufferShader;
    ASSERT_TRUE(program);
    mesh.submeshes[0].gbufferShader.reset();
    const std::vector<uint8_t> outside = shot(VIEW_OFF);
    mesh.submeshes[0].gbufferShader = program;
    const size_t boxPixel = pixelOf(Vector3(0.0f, 1.0f, -3.5f));
    std::vector<size_t> pixels;
    const int width = (int)GpuEnvironment::target().getWidth();
    for (int dy = -10; dy <= 10; dy++)
        for (int dx = -10; dx <= 10; dx++)
            pixels.push_back(boxPixel + (size_t)((long)dy * width + dx));
    ssrWriteImage("outside-gbuffer", outside);
    EXPECT_LE(ssrMeasured("a surface outside the G-buffer, under ACES", ssrMaxDiff(inGBuffer, outside, pixels)), 3);
    // and the clear colour comes back as it was set (stored as the code the curve turns into it)
    const size_t backgroundPixel = (size_t)width * 4 + 4;
    std::printf("[measured] the clear colour under ACES: %d, %d, %d (set: %ld)\n", (int)outside[backgroundPixel * 4],
                (int)outside[backgroundPixel * 4 + 1], (int)outside[backgroundPixel * 4 + 2], std::lround(0.08f * 255.0f));
    for (int c = 0; c < 2; c++)
        EXPECT_NEAR((int)outside[backgroundPixel * 4 + c], (int)std::lround(0.08f * 255.0f), 2);
    EXPECT_NEAR((int)outside[backgroundPixel * 4 + 2], (int)std::lround(0.1f * 255.0f), 2);
    EXPECT_NO_ENGINE_ERRORS();
}

// The resumed halves of the colour pass: the one after the composite does not load the colour the
// composite covers; every one another half follows keeps its depth and stencil (with DONTCARE
// GLES, WebGL and tilers drop them: the transparents would test against nothing)
TEST_F(SSRCompositeGpu, ResumedColourPassesKeepTheirDepth){
    JASPER_REQUIRE_GPU();
    build();
    // a reader of the scene colour over the floor: the pass splits again for its copy
    Primitive water(&scene);
    water.createPlane(4.0f, 4.0f);
    water.setPosition(0.0f, 0.05f, 2.0f);
    Material material;
    material.name = "Water";
    material.shader = "library:water";
    material.shaderFeatures = {"REFRACTION"};
    water.setMaterial(material);
    ASSERT_TRUE(render());
    const PassList list = ssrPasses();
    std::vector<size_t> camera;
    size_t afterMarch = list.passes.size();
    bool marched = false;
    for (size_t i = 0; i < list.passes.size(); i++){
        const std::string label = list.passes[i].label;
        if (label == "ssr")
            marched = true;
        if (label == "camera"){
            camera.push_back(i);
            if (marched && afterMarch == list.passes.size())
                afterMarch = i;
        }
    }
    ASSERT_GE(camera.size(), 3u) << "the colour pass in three halves (SSR, the copy)";
    ASSERT_LT(afterMarch, list.passes.size());
    EXPECT_EQ(list.passes[afterMarch].colorLoad[0], PassLoadAction::DontCare) << "the composite covers the colour";
    EXPECT_EQ(list.passes[afterMarch].depthLoad, PassLoadAction::Load);
    for (size_t k = 0; k + 1 < camera.size(); k++){
        const PassRecord& pass = list.passes[camera[k]];
        EXPECT_EQ(pass.depthStore, PassStoreAction::Store) << "half " << k << " of " << camera.size();
        EXPECT_EQ(pass.stencilStore, PassStoreAction::Store) << "half " << k << " of " << camera.size();
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// With the uber pass (FXAA here) the pass ends in the target the uber pass reads: no blit
TEST_F(SSRCompositeGpu, TheUberPassReadsTheSSRScene){
    JASPER_REQUIRE_GPU();
    PostProcessingSettings post = scene.getPostProcessing();
    post.fxaaEnabled = true;
    scene.setPostProcessing(post);
    build();
    ASSERT_TRUE(render());
    const std::vector<size_t> pixels = hit();
    ASSERT_GE(pixels.size(), MIN_HIT_PIXELS);
    const int bare = ssrMaxDiff(shot(VIEW_OFF), shot(VIEW_OFF, 0.0f), pixels);
    EXPECT_GE(bare, 30) << "the floor reflects the box";
    Primitive cover(&scene);
    ssrTransparentQuad(cover, 30.0f, 7.0f, Vector4(0.0f, 0.0f, 0.0f, 0.9f));
    cover.setPosition(0.0f, 0.02f, 0.0f);
    EXPECT_LE(ssrMeasured("reflection under the cover, uber pass", ssrMaxDiff(shot(VIEW_OFF), shot(VIEW_OFF, 0.0f), pixels)), bare / 6 + 2);
    const PassList list = ssrPasses();
    EXPECT_FALSE(ssrHasPass(list, "ssr scene blit"));
    EXPECT_TRUE(ssrHasPass(list, "ssr"));
    EXPECT_NO_ENGINE_ERRORS();
}

// The view's own target, of the pass's size, is where the pass continues: no target of its own,
// no blit; a camera drawing into a texture of another size takes the scene target and the blit
TEST_F(SSRCompositeGpu, ThePassContinuesInItsDestination){
    JASPER_REQUIRE_GPU();
    build();
    ASSERT_TRUE(render());
    EXPECT_FALSE(ssrHasPass(ssrPasses(), "ssr scene blit"));

    camera.setRenderToTexture(true);
    camera.setFramebufferSize(320, 180);
    ASSERT_TRUE(render(5));
    const PassList list = ssrPasses();
    EXPECT_TRUE(ssrHasPass(list, "ssr scene blit"));
    // the reflection reaches the camera's texture
    scene.setSSRDebugMode(VIEW_REFLECTION);
    ASSERT_TRUE(render(3));
    std::vector<uint8_t> rgba;
    Framebuffer* texture = camera.getFramebuffer();
    ASSERT_TRUE(texture);
    readTexture(texture->getRender().getColorTexture(), 320, 180, rgba);
    size_t white = 0;
    for (size_t p = 0; p < rgba.size() / 4; p++)
        white += (rgba[p * 4] >= 245 && rgba[p * 4 + 1] >= 245 && rgba[p * 4 + 2] >= 245) ? 1 : 0;
    EXPECT_GE(white, 50u) << "the box's reflection in the camera's texture";
    scene.setSSRDebugMode(VIEW_OFF);
    camera.setRenderToTexture(false);
    EXPECT_NO_ENGINE_ERRORS();
}

// Transparent Sort off: the pass draws in hierarchy order, no opaque/transparent boundary; SSR
// composites after it as before, and reflects
TEST_F(SSRCompositeGpu, AHierarchyOrderedPassCompositesAfterIt){
    JASPER_REQUIRE_GPU();
    camera.setTransparentSort(false);
    build();
    ASSERT_TRUE(render());
    const std::vector<size_t> pixels = hit();
    ASSERT_GE(pixels.size(), MIN_HIT_PIXELS);
    EXPECT_GE(ssrMaxDiff(shot(VIEW_OFF), shot(VIEW_OFF, 0.0f), pixels), 30);
    const PassList list = ssrPasses();
    EXPECT_TRUE(ssrHasPass(list, "ssr composite"));
    camera.setTransparentSort(true);
    EXPECT_NO_ENGINE_ERRORS();
}

// SSR off: the view's SSR targets go
TEST_F(SSRCompositeGpu, TurningSSROffFreesItsTargets){
    JASPER_REQUIRE_GPU();
    build();
    ASSERT_TRUE(render());
    const RenderStats on = SystemRender::getRenderStats(SystemRender::FRAME_STATS_FRAME);
    scene.setSSREnabled(false);
    ASSERT_TRUE(render(5));
    GpuEnvironment::drawFrames(2);
    const RenderStats off = SystemRender::getRenderStats(SystemRender::FRAME_STATS_FRAME);
    // the G-buffer (four targets and its depth), the scene colour and its depth, the march, the blur
    EXPECT_LE(off.aliveImages + 8, on.aliveImages) << on.aliveImages << " images with SSR, " << off.aliveImages << " without";
    EXPECT_NO_ENGINE_ERRORS();
}

// No sky, under the tone curve: the clear colour is stored as the code the curve turns back into
// it, so what shows of it through a library mesh's clipped pixels (solid in the G-buffer) comes out
// as it was set, as without SSR
TEST_F(SSRCompositeGpu, TheClearColourShowsThroughClippedHoles){
    JASPER_REQUIRE_GPU();
    PostProcessingSettings post = scene.getPostProcessing();
    post.tonemap = TonemapMode::ACES;
    post.exposure = 1.0f;
    scene.setPostProcessing(post);
    build(0.9f);
    // a quad in the background, every pixel clipped (its alpha under the cutoff)
    Primitive cutout(&scene);
    cutout.createPlane(4.0f, 2.0f);
    cutout.setRotation(90.0f, 0.0f, 0.0f);
    cutout.setPosition(0.0f, 3.0f, -8.0f);
    Material material;
    material.name = "Cut out";
    material.shader = "library:unlit";
    material.shaderFeatures = {"ALPHA_CLIP"};
    material.cullMode = MaterialCullMode::OFF;
    ShaderUniforms::set(material.shaderParams, "baseColor", Vector4(1.0f, 0.0f, 0.0f, 0.2f));
    ShaderUniforms::set(material.shaderParams, "alphaCutoff", Vector4(0.5f, 0.0f, 0.0f, 0.0f));
    cutout.setMaterial(material);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> on = shot(VIEW_OFF);
    scene.setSSREnabled(false);
    ASSERT_TRUE(render(5));
    const std::vector<uint8_t> off = ssrReadTarget();
    const size_t hole = pixelOf(Vector3(0.0f, 3.0f, -8.0f));
    std::vector<size_t> pixels;
    const int width = (int)GpuEnvironment::target().getWidth();
    for (int dy = -6; dy <= 6; dy++)
        for (int dx = -6; dx <= 6; dx++)
            pixels.push_back(hole + (size_t)((long)dy * width + dx));
    ssrWriteImage("clipped-holes", on);
    EXPECT_LE(ssrMeasured("the clear colour through clipped pixels, under ACES", ssrMaxDiff(on, off, pixels)), 2);
    EXPECT_NO_ENGINE_ERRORS();
}

// ... and on a multisampled edge against it: the resolve mixes stored codes only (the clear
// colour's too), so an outline pixel lies between the box and the clear colour on either side,
// no halo. (Its weighting differs from the image without SSR, whose resolve mixes display colour:
// the store's space is not the display's; printed, not a halo)
TEST_F(SSRCompositeGpu, MultisampledEdgesAgainstTheClearColourHaveNoHalo){
    JASPER_REQUIRE_GPU();
    PostProcessingSettings post = scene.getPostProcessing();
    post.tonemap = TonemapMode::ACES;
    post.exposure = 1.0f;
    post.msaaSamples = 4;
    scene.setPostProcessing(post);
    build(0.9f);
    // a lit box floating against the clear colour, turned so its sides slant across the pixels
    Primitive floating(&scene);
    floating.createBox(2.0f, 1.5f, 1.5f);
    floating.setPosition(0.0f, 3.0f, -8.0f);
    floating.setRotation(0.0f, 0.0f, 20.0f);
    Material material = floating.getMaterial();
    material.baseColorFactor = Vector4(0.9f, 0.8f, 0.3f, 1.0f);
    floating.setMaterial(material);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> on = shot(VIEW_OFF);
    scene.setSSREnabled(false);
    ASSERT_TRUE(render(5));
    const std::vector<uint8_t> off = ssrReadTarget();
    // the pixels on its outline (some of their samples on it, some on the clear colour)
    const int width = (int)GpuEnvironment::target().getWidth();
    const int height = (int)GpuEnvironment::target().getHeight();
    std::vector<size_t> edges;
    const int clearRed = (int)std::lround(0.08f * 255.0f);
    for (int y = 1; y < height - 1; y++){
        for (int x = 1; x < width - 1; x++){
            const size_t p = (size_t)y * width + x;
            const int r = off[p * 4];
            if (r > clearRed + 6 && r < (int)off[(p + 1) * 4] - 6 && off[(p - 1) * 4] <= clearRed + 1)
                edges.push_back(p);
            if (r > clearRed + 6 && r < (int)off[(p - 1) * 4] - 6 && off[(p + 1) * 4] <= clearRed + 1)
                edges.push_back(p);
        }
    }
    ASSERT_GE(edges.size(), 20u) << "the box's sides against the clear colour";
    ssrWriteImage("msaa-edges-with-ssr", on);
    ssrWriteImage("msaa-edges-without-ssr", off);
    ssrMeasured("multisampled edges against the clear colour, against the image without SSR", ssrMaxDiff(on, off, edges));
    // how far an outline pixel strays outside its two neighbours' range (a halo), in the image with SSR
    int halo = 0;
    for (size_t p : edges){
        for (int c = 0; c < 3; c++){
            const int left = on[(p - 1) * 4 + c];
            const int right = on[(p + 1) * 4 + c];
            const int own = on[p * 4 + c];
            halo = std::max(halo, std::max(std::min(left, right) - own, own - std::max(left, right)));
        }
    }
    EXPECT_LE(ssrMeasured("multisampled edges against the clear colour, outside their neighbours", halo), 3);
    EXPECT_NO_ENGINE_ERRORS();
}

// An exported build (no engine target: the swapchain) with the uber pass at a render scale under
// 1: the SSR targets are made once at the scaled size, not remade every frame
TEST_F(SSRCompositeGpu, ScaledSwapchainBuildsKeepTheirSSRTargets){
    JASPER_REQUIRE_GPU();
    PostProcessingSettings post = scene.getPostProcessing();
    post.renderScale = 0.5f;
    scene.setPostProcessing(post);
    build();
    ASSERT_TRUE(render());
    Engine::setFramebuffer(nullptr);
    GpuEnvironment::drawFrames(3);
    auto sceneColourImages = [](){
        std::vector<uint32_t> ids;
        for (const gpumem::Allocation& allocation : gpumem::report(true, false).allocations){
            if (allocation.image && (allocation.label.find("Scene colour") != std::string::npos ||
                    allocation.owner.find("Scene colour") != std::string::npos))
                ids.push_back(allocation.id);
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    };
    const std::vector<uint32_t> first = sceneColourImages();
    size_t remade = 0;
    for (int frame = 0; frame < 5; frame++){
        GpuEnvironment::drawFrames(1);
        remade += sceneColourImages() != first ? 1u : 0u;
    }
    Engine::setFramebuffer(&GpuEnvironment::target());
    ASSERT_TRUE(render(3));
    EXPECT_FALSE(first.empty()) << "SSR draws into a scene colour target";
    EXPECT_EQ(remade, 0u) << "frames whose scene colour target was made again";
    EXPECT_NO_ENGINE_ERRORS();
}
