// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The sky on the headless device (docs/specs/S94-rendering-anim-gaps.md F16-F18, F21):
//   - a hidden sky (Skybox.visible) and one whose entity is inactive draw nothing (AC16);
//   - the sky draws after the opaque queue, depth-tested, and before it without depth test (AC17);
//   - library:panoramic_sky's Per-Vertex Mapping matches the per-pixel mapping away from the
//     poles, with no atan, acos or derivative in its program (AC18);
//   - the panorama's rotation and the cube map's faces against Unity's Skybox/Panoramic and
//     Skybox/Cubemap through the mapping docs/unity-glossary.md gives (AC21).
// Pixels are compared, so the suite holds no golden; glcore and Vulkan alike.

#include "Engine.h"
#include "Scene.h"
#include "component/SkyboxComponent.h"
#include "object/Camera.h"
#include "object/Primitive.h"
#include "object/environment/Skybox.h"
#include "pool/ShaderPool.h"
#include "render/SystemRender.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"
#include "TempDir.h"

#include "stb_image_write.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr float SKY_PI = 3.14159265358979f;

    struct Rgb { int r, g, b; };

    bool writeImage(const std::string& path, int width, int height, const std::vector<uint8_t>& rgba){
        return stbi_write_png(path.c_str(), width, height, 4, rgba.data(), width * 4) != 0;
    }

    // a distinct, saturated colour per index (no two within 60 of each other on some channel)
    Rgb paletteColor(int index){
        static const Rgb colors[] = {
            {230, 30, 30}, {30, 230, 30}, {30, 30, 230}, {230, 230, 30}, {230, 30, 230}, {30, 230, 230},
            {240, 140, 30}, {140, 30, 240}, {30, 240, 140}, {140, 240, 30}, {240, 30, 140}, {30, 140, 240},
            {120, 120, 120}, {240, 240, 240}, {30, 30, 30}, {200, 90, 90}, {90, 200, 90}, {90, 90, 200},
            {200, 200, 90}, {200, 90, 200}, {90, 200, 200}, {160, 60, 0}, {0, 160, 60}, {60, 0, 160},
            {255, 180, 180}, {180, 255, 180}, {180, 180, 255}, {100, 50, 150}, {150, 100, 50}, {50, 150, 100},
            {255, 255, 120}, {120, 255, 255}};
        return colors[index % (int)(sizeof(colors) / sizeof(colors[0]))];
    }

    bool near(const uint8_t* pixel, Rgb color, int tolerance){
        return std::abs(pixel[0] - color.r) <= tolerance && std::abs(pixel[1] - color.g) <= tolerance &&
               std::abs(pixel[2] - color.b) <= tolerance;
    }

    std::string describe(const uint8_t* pixel){
        return "(" + std::to_string(pixel[0]) + ", " + std::to_string(pixel[1]) + ", " + std::to_string(pixel[2]) + ")";
    }

    // Unity's RotateAroundYInDegrees (Skybox/Panoramic, Skybox/Cubemap): (x, z) -> (c x - s z, s x + c z)
    Vector3 unityRotate(const Vector3& v, float degrees){
        const float a = degrees * SKY_PI / 180.0f;
        return Vector3(std::cos(a) * v.x - std::sin(a) * v.z, v.y, std::sin(a) * v.x + std::cos(a) * v.z);
    }

    // a Unity world direction as the engine's: the X mirror of the Unity importers (S = diag(-1, 1, 1))
    Vector3 fromUnity(const Vector3& u){
        return Vector3(-u.x, u.y, u.z);
    }

    // the GPU cube map rule (the same on every API): the face a direction reads, and its texel
    // coordinates (u across, v down from the face's first row), 0..1
    void cubeLookup(const Vector3& d, int& face, float& u, float& v){
        const float ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
        float sc, tc, ma;
        if (ax >= ay && ax >= az){
            face = d.x > 0.0f ? 0 : 1;
            ma = ax;
            sc = d.x > 0.0f ? -d.z : d.z;
            tc = -d.y;
        }else if (ay >= az){
            face = d.y > 0.0f ? 2 : 3;
            ma = ay;
            sc = d.x;
            tc = d.y > 0.0f ? d.z : -d.z;
        }else{
            face = d.z > 0.0f ? 4 : 5;
            ma = az;
            sc = d.z > 0.0f ? d.x : -d.x;
            tc = -d.y;
        }
        u = 0.5f * (sc / ma + 1.0f);
        v = 0.5f * (tc / ma + 1.0f);
    }

    // the direction of a face's texel coordinate (cubeLookup's inverse)
    Vector3 cubeDirection(int face, float u, float v){
        const float sc = 2.0f * u - 1.0f;
        const float tc = 2.0f * v - 1.0f;
        switch (face){
            case 0: return Vector3(1.0f, -tc, -sc);
            case 1: return Vector3(-1.0f, -tc, sc);
            case 2: return Vector3(sc, 1.0f, tc);
            case 3: return Vector3(sc, -1.0f, -tc);
            case 4: return Vector3(sc, -tc, 1.0f);
            default: return Vector3(-sc, -tc, -1.0f);
        }
    }

    class SkyGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        TempDir dir;

        void SetUp() override{
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setYFov(40.0f);
            camera.setPosition(0.0f, 0.0f, 0.0f);
            camera.setTarget(0.0f, 0.0f, -1.0f);
            camera.activate();
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
            SystemRender::setDrawListEnabled(false);
        }

        bool render(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }

        std::vector<uint8_t> readTarget(){
            Framebuffer& target = GpuEnvironment::target();
            std::vector<uint8_t> rgba;
            readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
            return rgba;
        }

        std::array<uint8_t, 4> centerPixel(){
            GpuEnvironment::drawFrames(2);
            const std::vector<uint8_t> rgba = readTarget();
            Framebuffer& target = GpuEnvironment::target();
            const int w = (int)target.getWidth();
            const int h = (int)target.getHeight();
            std::array<uint8_t, 4> pixel{};
            if (rgba.size() < (size_t)w * h * 4)
                return pixel;
            const size_t i = ((size_t)(h / 2) * w + (size_t)(w / 2)) * 4;
            std::copy(rgba.begin() + (std::ptrdiff_t)i, rgba.begin() + (std::ptrdiff_t)i + 4, pixel.begin());
            return pixel;
        }

        void lookAt(const Vector3& direction){
            camera.setPosition(0.0f, 0.0f, 0.0f);
            camera.setTarget(direction.x, direction.y, direction.z);
        }

        // a panorama of 8 x 4 solid blocks (paletteColor(row * 8 + column)), 256 x 128
        std::string blockPanorama(){
            const int w = 256, h = 128;
            std::vector<uint8_t> rgba((size_t)w * h * 4, 255);
            for (int y = 0; y < h; y++){
                for (int x = 0; x < w; x++){
                    const Rgb c = paletteColor((y / 32) * 8 + (x / 32));
                    uint8_t* p = &rgba[((size_t)y * w + x) * 4];
                    p[0] = (uint8_t)c.r; p[1] = (uint8_t)c.g; p[2] = (uint8_t)c.b;
                }
            }
            const std::string path = dir.str() + "/blocks.png";
            EXPECT_TRUE(writeImage(path, w, h, rgba)) << path;
            return path;
        }
    };

}

// AC16 (F16): Skybox.visible false, then an inactive sky entity, each show the clear colour
TEST_F(SkyGpu, HiddenOrInactiveSkyDrawsNothing){
    JASPER_REQUIRE_GPU();
    scene.setBackgroundColor(0.0f, 0.0f, 1.0f, 1.0f);
    Skybox sky(&scene);
    sky.setColor(1.0f, 0.0f, 0.0f);
    ASSERT_TRUE(render());
    std::array<uint8_t, 4> pixel = centerPixel();
    EXPECT_GT(pixel[0], 200) << "the sky draws " << describe(pixel.data());

    sky.setVisible(false);
    EXPECT_FALSE(sky.isVisible());
    pixel = centerPixel();
    EXPECT_TRUE(near(pixel.data(), Rgb{0, 0, 255}, 2)) << "a hidden sky shows the clear colour " << describe(pixel.data());

    sky.setVisible(true);
    sky.setActive(false);
    pixel = centerPixel();
    EXPECT_TRUE(near(pixel.data(), Rgb{0, 0, 255}, 2)) << "an inactive sky shows the clear colour " << describe(pixel.data());

    sky.setActive(true);
    pixel = centerPixel();
    EXPECT_GT(pixel[0], 200) << "active again, it draws " << describe(pixel.data());
    EXPECT_NO_ENGINE_ERRORS();
}

// AC17 (F17): the sky's draw comes after the opaque queue's, under the depth test (the opaque
// sphere keeps its pixels); a camera without depth test draws it first, as the background
TEST_F(SkyGpu, DrawsAfterTheOpaqueQueue){
    JASPER_REQUIRE_GPU();
    Skybox sky(&scene);
    sky.setColor(1.0f, 0.0f, 0.0f);
    Primitive ball(&scene);
    ball.createSphere(0.5f, 24, 12);
    ball.setColor(0.0f, 1.0f, 0.0f);
    ball.setPosition(0.0f, 0.0f, -3.0f);
    ASSERT_TRUE(render());

    auto skyDrawIndex = [&](int& meshIndex){
        SystemRender::setDrawListEnabled(true);
        GpuEnvironment::drawFrames(2);
        const PassList list = SystemRender::getPassList(SystemRender::FRAME_STATS_FRAME, false);
        SystemRender::setDrawListEnabled(false);
        int skyIndex = -1;
        meshIndex = -1;
        for (size_t i = 0; i < list.draws.size(); i++){
            const DrawRecord& draw = list.draws[i];
            // the sky's 36 vertices (the cube), the sphere's 24 x 12 grid's indices
            if (draw.elements == 36 && skyIndex < 0)
                skyIndex = (int)i;
            else if (draw.elements > 36 && meshIndex < 0)
                meshIndex = (int)i;
        }
        return skyIndex;
    };

    int meshIndex = -1;
    int skyIndex = skyDrawIndex(meshIndex);
    ASSERT_GE(skyIndex, 0) << "the sky's draw is in the list";
    ASSERT_GE(meshIndex, 0) << "the sphere's draw is in the list";
    EXPECT_GT(skyIndex, meshIndex) << "the sky draws after the opaque queue";
    std::array<uint8_t, 4> pixel = centerPixel();
    EXPECT_TRUE(near(pixel.data(), Rgb{0, 255, 0}, 40)) << "the sphere covers the centre, depth-tested " << describe(pixel.data());

    // no depth test: hierarchy order, the sky the background
    camera.setDepthTest(false);
    ASSERT_TRUE(render());
    skyIndex = skyDrawIndex(meshIndex);
    ASSERT_GE(skyIndex, 0);
    ASSERT_GE(meshIndex, 0);
    EXPECT_LT(skyIndex, meshIndex) << "without depth test the sky draws first";
    pixel = centerPixel();
    EXPECT_TRUE(near(pixel.data(), Rgb{0, 255, 0}, 40)) << describe(pixel.data());
    EXPECT_NO_ENGINE_ERRORS();
}

// AC21 (F21): Unity's Skybox/Panoramic at _Rotation r is library:panoramic_sky at rotation
// (r + 270) mod 360 (docs/unity-glossary.md). For engine rotations 0, 90 and 200, the block
// Unity's formula puts in a direction is the one the centre of a camera looking there shows.
TEST_F(SkyGpu, PanoramaRotationMatchesUnity){
    JASPER_REQUIRE_GPU();
    Skybox sky(&scene);
    sky.setCustomShader("library:panoramic_sky");
    sky.setShaderTexture("u_panoramaTexture", blockPanorama());
    for (float rotation : {0.0f, 90.0f, 200.0f}){
        const float unityRotation = std::fmod(rotation + 90.0f, 360.0f);
        sky.setShaderUniform("rotation", rotation);
        ASSERT_TRUE(render());
        for (int block : {3, 10, 13, 22}){
            const int column = block % 8;
            const int row = block / 8;
            const float u = ((float)column + 0.5f) / 8.0f;
            const float v = ((float)row + 0.5f) / 4.0f;
            // Unity's ToRadialCoords: u = 0.5 - atan2(s.z, s.x) / 2pi, the top row straight up
            const float longitude = (0.5f - u) * 2.0f * SKY_PI;
            const float polar = v * SKY_PI;
            const Vector3 sampled(std::sin(polar) * std::cos(longitude), std::cos(polar), std::sin(polar) * std::sin(longitude));
            // s = R(rotation) world: the world direction is its inverse rotation
            const Vector3 world = fromUnity(unityRotate(sampled, -unityRotation));
            lookAt(world);
            const std::array<uint8_t, 4> pixel = centerPixel();
            EXPECT_TRUE(near(pixel.data(), paletteColor(block), 24))
                << "rotation " << rotation << " (Unity " << unityRotation << "), block " << block << ": " << describe(pixel.data());
        }
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// AC21 (F21): Unity's Skybox/Cubemap faces load as the engine's with +X and -X swapped and every
// face mirrored left to right, and its _Rotation r is Skybox.rotation -r (docs/unity-glossary.md).
// Each face holds four quadrants of their own colour; the centre of a camera looking where
// Unity's sampling puts a quadrant shows it.
TEST_F(SkyGpu, CubemapFacesMatchUnity){
    JASPER_REQUIRE_GPU();
    const int size = 32;
    // Unity's faces, +X -X +Y -Y +Z -Z: quadrant q (0 top-left, 1 top-right, 2 bottom-left, 3
    // bottom-right) of face f is paletteColor(f * 4 + q)
    auto unityTexel = [](int face, float u, float v){
        return paletteColor(face * 4 + (v < 0.5f ? 0 : 2) + (u < 0.5f ? 0 : 1));
    };
    // the engine's faces by the documented recipe
    static const int unityFaceOf[6] = {1, 0, 2, 3, 4, 5};
    std::string paths[6];
    for (int face = 0; face < 6; face++){
        std::vector<uint8_t> rgba((size_t)size * size * 4, 255);
        for (int y = 0; y < size; y++){
            for (int x = 0; x < size; x++){
                // mirrored: the engine's column x is Unity's column size - 1 - x
                const Rgb c = unityTexel(unityFaceOf[face], ((float)(size - 1 - x) + 0.5f) / size, ((float)y + 0.5f) / size);
                uint8_t* p = &rgba[((size_t)y * size + x) * 4];
                p[0] = (uint8_t)c.r; p[1] = (uint8_t)c.g; p[2] = (uint8_t)c.b;
            }
        }
        paths[face] = dir.str() + "/face" + std::to_string(face) + ".png";
        ASSERT_TRUE(writeImage(paths[face], size, size, rgba)) << paths[face];
    }
    Skybox sky(&scene);
    sky.setTextures(paths[0], paths[1], paths[2], paths[3], paths[4], paths[5]);
    for (float unityRotation : {0.0f, 90.0f, 200.0f}){
        sky.setRotation(-unityRotation);
        ASSERT_TRUE(render());
        for (int face = 0; face < 6; face++){
            for (int quadrant : {0, 3}){
                const float u = (quadrant % 2 == 0) ? 0.25f : 0.75f;
                const float v = (quadrant < 2) ? 0.25f : 0.75f;
                // where Unity samples this quadrant: s = R(rotation) world
                const Vector3 sampled = cubeDirection(face, u, v);
                const Vector3 world = fromUnity(unityRotate(sampled, -unityRotation));
                int checkFace;
                float cu, cv;
                cubeLookup(sampled, checkFace, cu, cv);
                ASSERT_EQ(checkFace, face);
                lookAt(world);
                const std::array<uint8_t, 4> pixel = centerPixel();
                EXPECT_TRUE(near(pixel.data(), unityTexel(face, cu, cv), 24))
                    << "Unity rotation " << unityRotation << ", face " << face << ", quadrant " << quadrant << ": " << describe(pixel.data());
            }
        }
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// AC18 (F18): Per-Vertex Mapping draws what the per-pixel mapping draws within 3/255, in views
// reaching 65 degrees above and below the horizon (the grid's pole fans, within about 6 degrees
// of the poles, are left out), with one textureLod read and no atan, acos or derivative
TEST_F(SkyGpu, PanoramaVertexMapping){
    JASPER_REQUIRE_GPU();
    // a smooth panorama (a block image would differ by whole colours at sub-texel offsets)
    const int w = 512, h = 256;
    std::vector<uint8_t> rgba((size_t)w * h * 4, 255);
    for (int y = 0; y < h; y++){
        for (int x = 0; x < w; x++){
            const float u = ((float)x + 0.5f) / w;
            const float v = ((float)y + 0.5f) / h;
            uint8_t* p = &rgba[((size_t)y * w + x) * 4];
            p[0] = (uint8_t)std::lround(127.5f + 120.0f * std::sin(2.0f * SKY_PI * u * 3.0f));
            p[1] = (uint8_t)std::lround(127.5f + 120.0f * std::cos(SKY_PI * v * 4.0f));
            p[2] = (uint8_t)std::lround(127.5f + 120.0f * std::sin(2.0f * SKY_PI * (u + v)));
        }
    }
    const std::string path = dir.str() + "/smooth.png";
    ASSERT_TRUE(writeImage(path, w, h, rgba)) << path;
    Skybox sky(&scene);
    sky.setCustomShader("library:panoramic_sky");
    sky.setShaderTexture("u_panoramaTexture", path);
    sky.setShaderUniform("rotation", 30.0f);
    sky.setRotation(15.0f);

    struct View { float yaw, pitch; };
    const View views[] = {{0.0f, 0.0f}, {90.0f, 35.0f}, {200.0f, -35.0f}, {300.0f, 10.0f}};
    for (const View& view : views){
        const float yaw = view.yaw * SKY_PI / 180.0f;
        const float pitch = view.pitch * SKY_PI / 180.0f;
        lookAt(Vector3(std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)));
        sky.setShaderFeature("VERTEX_MAPPING", false);
        ASSERT_TRUE(render());
        GpuEnvironment::drawFrames(2);
        const std::vector<uint8_t> perPixel = readTarget();
        sky.setShaderFeature("VERTEX_MAPPING", true);
        ASSERT_TRUE(render());
        GpuEnvironment::drawFrames(2);
        const std::vector<uint8_t> perVertex = readTarget();
        ASSERT_EQ(perPixel.size(), perVertex.size());
        int worst = 0;
        for (size_t i = 0; i + 3 < perPixel.size(); i += 4){
            for (int c = 0; c < 3; c++)
                worst = std::max(worst, std::abs((int)perPixel[i + c] - (int)perVertex[i + c]));
        }
        EXPECT_LE(worst, 3) << "yaw " << view.yaw << ", pitch " << view.pitch;
    }
    EXPECT_EQ(scene.getComponent<SkyboxComponent>(sky.getEntity()).vertexCount, 64u * 30u * 6u + 2u * 64u * 3u) << "the sphere grid";

    // the variant's program: one textureLod, no atan, acos or derivatives
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    const std::string shader = "library:panoramic_sky#VERTEX_MAPPING";
    const CustomShaderSources sources = ShaderVariantCompiler::resolveCustomShaderSources(
        shader, [](const std::string&, std::string&){ return false; }, ShaderType::SKYBOX);
    ASSERT_TRUE(sources.valid) << sources.error;
    const ShaderKey key = ShaderPool::getShaderKey(ShaderType::SKYBOX, 0, ShaderPool::registerCustomShader(shader));
    std::string vertex, fragment, error;
    ASSERT_TRUE(ShaderVariantCompiler::preprocess(key, ShaderBackend::GLCore, &sources, vertex, fragment, &error)) << error;
    for (const char* banned : {"atan(", "acos(", "dFdx(", "dFdy(", "textureGrad("})
        EXPECT_EQ(fragment.find(banned), std::string::npos) << banned;
    EXPECT_NE(fragment.find("textureLod("), std::string::npos);
    EXPECT_NE(vertex.find("a_texcoord1"), std::string::npos) << "the grid's coordinate";
    EXPECT_NO_ENGINE_ERRORS();
}
