// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The sun of an HDR sky (spec S99 AC7, R28, R29): found in synthetic equirects and cube faces
// within a degree, its light measured, a sky without a distinct sun refused, and Remove Sun Disc
// clamping the disc alone, in the analysis and in the cook's decode.

#include <gtest/gtest.h>

#include "lighting/HdriSun.h"

#include "support/TextureCorpus.h"
#include "texture/TextureCooker.h"
#include "texture/TextureData.h"

#include "stb_image_write.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <vector>

namespace HdriSunTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::hdrisun;
namespace fs = std::filesystem;

namespace {

    constexpr float kHdriSunTestPi = 3.14159265358979323846f;

    Vector3 fromAngles(float azimuthDegrees, float elevationDegrees){
        const float a = azimuthDegrees * kHdriSunTestPi / 180.0f;
        const float e = elevationDegrees * kHdriSunTestPi / 180.0f;
        return Vector3(std::cos(e) * std::sin(a), std::sin(e), std::cos(e) * std::cos(a));
    }

    float hdriSunAngleDegrees(const Vector3& a, const Vector3& b){
        const float c = std::clamp(a.dotProduct(b) / (a.length() * b.length()), -1.0f, 1.0f);
        return std::acos(c) * 180.0f / kHdriSunTestPi;
    }

    // a clear sky: a blue gradient over a darker ground, and a sun disc of `sunRadius` degrees
    // and radiance `sunRadiance` (0: none)
    struct SyntheticSky {
        Vector3 sun = fromAngles(120.0f, 35.0f);
        float sunRadius = 1.0f;
        float sunRadiance = 2000.0f;

        Vector3 radiance(const Vector3& d) const{
            Vector3 sky = d.y >= 0.0f ? Vector3(0.4f, 0.6f, 1.0f) * (0.6f + 0.4f * d.y) : Vector3(0.15f, 0.12f, 0.1f);
            if (sunRadiance > 0.0f && hdriSunAngleDegrees(d, sun) <= sunRadius)
                sky = Vector3(sunRadiance, sunRadiance * 0.95f, sunRadiance * 0.85f);
            return sky;
        }

        // 4x4 samples a texel, so the disc's edge is partly covered as a camera would see it
        std::vector<float> equirect(int width, int height) const{
            std::vector<float> rgba((size_t)width * height * 4);
            for (int y = 0; y < height; y++){
                for (int x = 0; x < width; x++){
                    Vector3 sum;
                    for (int s = 0; s < 16; s++){
                        const float lon = ((float)x + 0.125f + 0.25f * (float)(s & 3)) / (float)width * 2.0f * kHdriSunTestPi - kHdriSunTestPi;
                        const float lat = 0.5f * kHdriSunTestPi - ((float)y + 0.125f + 0.25f * (float)(s >> 2)) / (float)height * kHdriSunTestPi;
                        sum += radiance(Vector3(std::cos(lat) * std::sin(lon), std::sin(lat), std::cos(lat) * std::cos(lon)));
                    }
                    float* p = &rgba[((size_t)y * width + x) * 4];
                    p[0] = sum.x / 16.0f;
                    p[1] = sum.y / 16.0f;
                    p[2] = sum.z / 16.0f;
                    p[3] = 1.0f;
                }
            }
            return rgba;
        }

        // the engine's cube face convention (IBLEnvironment): 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z
        static Vector3 faceDirection(int face, float u, float v){
            Vector3 d;
            switch (face){
                case 0: d = Vector3(1.0f, -v, -u); break;
                case 1: d = Vector3(-1.0f, -v, u); break;
                case 2: d = Vector3(u, 1.0f, v); break;
                case 3: d = Vector3(u, -1.0f, -v); break;
                case 4: d = Vector3(u, -v, 1.0f); break;
                default: d = Vector3(-u, -v, -1.0f); break;
            }
            return d.normalized();
        }

        std::vector<float> face(int index, int size) const{
            std::vector<float> rgba((size_t)size * size * 4);
            for (int y = 0; y < size; y++){
                for (int x = 0; x < size; x++){
                    Vector3 sum;
                    for (int s = 0; s < 16; s++){
                        const float u = 2.0f * ((float)x + 0.125f + 0.25f * (float)(s & 3)) / (float)size - 1.0f;
                        const float v = 2.0f * ((float)y + 0.125f + 0.25f * (float)(s >> 2)) / (float)size - 1.0f;
                        sum += radiance(faceDirection(index, u, v));
                    }
                    float* p = &rgba[((size_t)y * size + x) * 4];
                    p[0] = sum.x / 16.0f;
                    p[1] = sum.y / 16.0f;
                    p[2] = sum.z / 16.0f;
                    p[3] = 1.0f;
                }
            }
            return rgba;
        }

        // the sun's irradiance above the sky behind it: radiance x the disc's solid angle
        float discIrradiance() const{
            return sunRadiance * 2.0f * kHdriSunTestPi * (1.0f - std::cos(sunRadius * kHdriSunTestPi / 180.0f));
        }
    };

    SkyImage equirectView(std::vector<float>& rgba, int width, int height){
        SkyImage view;
        view.rgba = rgba.data();
        view.width = width;
        view.height = height;
        view.layout = SkyLayout::Equirect;
        return view;
    }

    float maxLuminance(const std::vector<float>& rgba){
        float best = 0.0f;
        for (size_t i = 0; i + 3 < rgba.size(); i += 4)
            best = std::max(best, 0.2126f * rgba[i] + 0.7152f * rgba[i + 1] + 0.0722f * rgba[i + 2]);
        return best;
    }

}

TEST(HdriSun, FindsTheSunOfSyntheticEquirectsWithinADegree){
    const Vector3 directions[] = {fromAngles(120.0f, 35.0f), fromAngles(-60.0f, 8.0f), fromAngles(10.0f, 70.0f)};
    for (const Vector3& direction : directions){
        SyntheticSky sky;
        sky.sun = direction;
        std::vector<float> rgba = sky.equirect(512, 256);
        SunResult sun;
        std::string error;
        ASSERT_TRUE(findSun({equirectView(rgba, 512, 256)}, sun, error)) << error;
        EXPECT_LT(hdriSunAngleDegrees(sun.direction, direction), 1.0f);
        EXPECT_GE(sun.peakRatio, MIN_PEAK_RATIO);
        // the disc's light, its colour, and a disc no wider than the sun's and a texel
        EXPECT_NEAR(sun.irradiance.x, sky.discIrradiance(), sky.discIrradiance() * 0.15f);
        EXPECT_NEAR(sun.irradiance.z / sun.irradiance.x, 0.85f, 0.03f);
        EXPECT_GE(sun.discDegrees, sky.sunRadius);
        EXPECT_LE(sun.discDegrees, sky.sunRadius + 1.5f);
    }
}

TEST(HdriSun, FindsTheSunAcrossCubeFaces){
    SyntheticSky sky;
    // near the +X / +Z edge, so the disc spans two faces
    sky.sun = fromAngles(44.0f, 20.0f);
    std::array<std::vector<float>, 6> faces;
    std::vector<SkyImage> views;
    for (int f = 0; f < 6; f++){
        faces[f] = sky.face(f, 128);
        SkyImage view;
        view.rgba = faces[f].data();
        view.width = 128;
        view.height = 128;
        view.face = f;
        views.push_back(view);
    }
    SunResult sun;
    std::string error;
    ASSERT_TRUE(findSun(views, sun, error)) << error;
    EXPECT_LT(hdriSunAngleDegrees(sun.direction, sky.sun), 1.0f);
    EXPECT_NEAR(sun.irradiance.x, sky.discIrradiance(), sky.discIrradiance() * 0.15f);
}

TEST(HdriSun, RefusesASkyWithoutADistinctSun){
    SyntheticSky overcast;
    overcast.sunRadiance = 0.0f;
    std::vector<float> rgba = overcast.equirect(256, 128);
    SunResult sun;
    std::string error;
    EXPECT_FALSE(findSun({equirectView(rgba, 256, 128)}, sun, error));
    EXPECT_NE(error.find("no distinct sun"), std::string::npos) << error;
    EXPECT_NE(error.find("20"), std::string::npos) << error;

    // a dim sun, 10x the sky: still none
    SyntheticSky hazy;
    hazy.sunRadiance = 4.0f;
    hazy.sunRadius = 2.0f;
    rgba = hazy.equirect(256, 128);
    EXPECT_FALSE(findSun({equirectView(rgba, 256, 128)}, sun, error));
}

TEST(HdriSun, RemoveSunDiscClampsTheDiscAlone){
    SyntheticSky sky;
    std::vector<float> rgba = sky.equirect(512, 256);
    const std::vector<float> before = rgba;
    std::string why;
    ASSERT_TRUE(removeSunDiscInPlace(rgba.data(), 512, 256, false, why)) << why;
    // the disc is the sky around it now, and the sky away from it is as it was
    EXPECT_LT(maxLuminance(rgba), 1.5f);
    size_t changedFar = 0;
    for (int y = 0; y < 256; y++){
        for (int x = 0; x < 512; x++){
            const float lon = ((float)x + 0.5f) / 512.0f * 2.0f * kHdriSunTestPi - kHdriSunTestPi;
            const float lat = 0.5f * kHdriSunTestPi - ((float)y + 0.5f) / 256.0f * kHdriSunTestPi;
            const Vector3 d(std::cos(lat) * std::sin(lon), std::sin(lat), std::cos(lat) * std::cos(lon));
            const size_t i = ((size_t)y * 512 + x) * 4;
            if (hdriSunAngleDegrees(d, sky.sun) > MAX_DISC_DEGREES + 1.0f && rgba[i] != before[i])
                changedFar++;
        }
    }
    EXPECT_EQ(changedFar, 0u);
    // no sun is left to find
    SunResult sun;
    EXPECT_FALSE(findSun({equirectView(rgba, 512, 256)}, sun, why));

    // neither a cube face, an equirect nor a cross
    std::vector<float> odd(300 * 100 * 4, 1.0f);
    EXPECT_FALSE(removeSunDiscInPlace(odd.data(), 300, 100, false, why));
    EXPECT_FALSE(removeSunDiscInPlace(odd.data(), 300, 100, true, why));
}

// the cook's decode (TextureCooker::decodeSource with Remove Sun Disc): an .hdr's disc is clamped
// before anything else reads it; an image without a sun is left as it is, with a note why
TEST(HdriSun, CookDecodeRemovesTheDiscOfAnHdrSource){
    jasperengine::editor::test::ScratchDir scratch("hdri-sun-cook");
    auto writeHdr = [&](const std::string& name, const std::vector<float>& rgba, int width, int height){
        std::vector<float> rgb((size_t)width * height * 3);
        for (size_t i = 0; i < (size_t)width * height; i++)
            for (int c = 0; c < 3; c++)
                rgb[i * 3 + c] = rgba[i * 4 + c];
        const fs::path path = scratch.path / name;
        EXPECT_TRUE(stbi_write_hdr(path.string().c_str(), width, height, 3, rgb.data()));
        return path;
    };
    SyntheticSky sky;
    const fs::path withSun = writeHdr("sun.hdr", sky.equirect(256, 128), 256, 128);
    std::array<TextureData, 6> faces;
    std::string error, note;
    ASSERT_EQ(TextureCooker::decodeSource(withSun, false, faces, error), 1) << error;
    const float* kept = (const float*)faces[0].getData();
    const std::vector<float> asIs(kept, kept + 256 * 128 * 4);
    EXPECT_GT(maxLuminance(asIs), 100.0f) << "without the setting the sun stays";
    faces[0].releaseImageData();

    ASSERT_EQ(TextureCooker::decodeSource(withSun, false, faces, error, true, &note), 1) << error;
    EXPECT_TRUE(note.empty()) << note;
    const float* removed = (const float*)faces[0].getData();
    EXPECT_LT(maxLuminance(std::vector<float>(removed, removed + 256 * 128 * 4)), 1.5f);
    faces[0].releaseImageData();

    SyntheticSky overcast;
    overcast.sunRadiance = 0.0f;
    const fs::path noSun = writeHdr("overcast.hdr", overcast.equirect(256, 128), 256, 128);
    ASSERT_EQ(TextureCooker::decodeSource(noSun, false, faces, error, true, &note), 1) << error;
    EXPECT_NE(note.find("no distinct sun"), std::string::npos) << note;
    faces[0].releaseImageData();
}

}  // namespace HdriSunTest_cpp
