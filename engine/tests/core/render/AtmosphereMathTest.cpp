// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The height fog's medium (AtmosphereMath, mirrored by shaderlib/includes/height_fog.glsl): the
// closed-form optical depth against a numeric integration of the density, the composition of
// ray segments the volumetric fog's handoff relies on (docs/specs/S12-atmosphere.md AC3), the
// phase function, the Sky Map's hemi-octahedral mapping and the Local Volumetric Fog volumes'
// density and selection (AC7).

#include "render/AtmosphereMath.h"
#include "component/CameraComponent.h"
#include "math/Quaternion.h"

#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <vector>

using namespace jasperengine;

namespace {

    // the optical depth by the midpoint rule over steps parts, start and max distances applied
    double numericOpticalDepth(const HeightFogMedium& medium, const Vector3& origin, const Vector3& dir, float t0, float t1, int steps){
        const double a = std::max(t0, medium.startDistance);
        const double b = std::min(t1, medium.maxDistance);
        if (b <= a)
            return 0.0;
        const double dt = (b - a) / steps;
        double sum = 0.0;
        for (int i = 0; i < steps; i++){
            const double t = a + (i + 0.5) * dt;
            const double height = origin.y + t * dir.y;
            const double above = height - medium.baseHeight;
            sum += above <= 0.0 ? 1.0 : std::exp(-above / medium.scaleHeight);
        }
        return sum * dt * medium.extinction;
    }

    FogComponent heightFog(){
        FogComponent fog;
        fog.type = FogType::HEIGHT;
        fog.attenuationDistance = 150.0f;
        fog.baseHeight = 10.0f;
        fog.maximumHeight = 90.0f;
        fog.startDistance = 0.0f;
        fog.maxFogDistance = 3000.0f;
        return fog;
    }

    Vector3 randomDirection(std::mt19937& rng, float minY, float maxY){
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        const float y = minY + (maxY - minY) * unit(rng);
        const float azimuth = 6.2831853f * unit(rng);
        const float flat = std::sqrt(std::max(1.0f - y * y, 0.0f));
        return Vector3(flat * std::cos(azimuth), y, flat * std::sin(azimuth));
    }

    FogVolumeData boxVolume(uint32_t entity, const Vector3& center, const Vector3& size){
        FogVolumeData volume;
        volume.entity = entity;
        volume.fog.size = size;
        volume.worldToVolume = Matrix4::translateMatrix(center).inverse();
        volume.halfExtents = size * 0.5f;
        volume.center = center;
        volume.boundingRadius = volume.halfExtents.length();
        return volume;
    }

    // a view looking down -Z from the origin: x and y within 50 m, 0 to 100 m deep
    void boxFrustum(Plane planes[6]){
        planes[FRUSTUM_PLANE_NEAR] = Plane(0.0f, 0.0f, -1.0f, 0.0f);
        planes[FRUSTUM_PLANE_FAR] = Plane(0.0f, 0.0f, 1.0f, 100.0f);
        planes[FRUSTUM_PLANE_LEFT] = Plane(1.0f, 0.0f, 0.0f, 50.0f);
        planes[FRUSTUM_PLANE_RIGHT] = Plane(-1.0f, 0.0f, 0.0f, 50.0f);
        planes[FRUSTUM_PLANE_BOTTOM] = Plane(0.0f, 1.0f, 0.0f, 50.0f);
        planes[FRUSTUM_PLANE_TOP] = Plane(0.0f, -1.0f, 0.0f, 50.0f);
    }

}

TEST(AtmosphereMath, ScaleHeightThinsToAThousandthAtTheMaximumHeight){
    const HeightFogMedium medium = AtmosphereMath::makeMedium(heightFog());
    EXPECT_FLOAT_EQ(AtmosphereMath::density(medium, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(AtmosphereMath::density(medium, 10.0f), 1.0f);
    EXPECT_NEAR(AtmosphereMath::density(medium, 90.0f), 0.001f, 1e-6f);
    EXPECT_FLOAT_EQ(medium.extinction, 1.0f / 150.0f);
}

TEST(AtmosphereMath, OpticalDepthMatchesNumericIntegration){
    const HeightFogMedium medium = AtmosphereMath::makeMedium(heightFog());
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> length(5.0f, 2500.0f);
    // camera below and above the base height; rays down, level and up
    const float heights[2] = {2.0f, 60.0f};
    const float bands[3][2] = {{-0.9f, -0.05f}, {-0.02f, 0.02f}, {0.05f, 0.9f}};
    int rays = 0;
    for (int i = 0; i < 200; i++){
        const Vector3 origin(0.0f, heights[i % 2], 0.0f);
        const float* band = bands[(i / 2) % 3];
        const Vector3 dir = randomDirection(rng, band[0], band[1]);
        const float t1 = length(rng);
        const double closed = AtmosphereMath::opticalDepth(medium, origin, dir, 0.0f, t1);
        const double numeric = numericOpticalDepth(medium, origin, dir, 0.0f, t1, 10000);
        ASSERT_GT(numeric, 0.0);
        EXPECT_NEAR(closed, numeric, numeric * 0.005) << "ray " << i << " from y " << origin.y << " dir " << dir.toString() << " length " << t1;
        rays++;
    }
    EXPECT_EQ(rays, 200);
}

TEST(AtmosphereMath, SegmentsCompose){
    FogComponent fog = heightFog();
    fog.startDistance = 25.0f;
    fog.maxFogDistance = 900.0f;
    const HeightFogMedium medium = AtmosphereMath::makeMedium(fog);
    std::mt19937 rng(777);
    std::uniform_real_distribution<float> split(0.0f, 1.0f);
    for (int i = 0; i < 200; i++){
        const Vector3 origin(0.0f, i % 2 ? 3.0f : 40.0f, 0.0f);
        const Vector3 dir = randomDirection(rng, -0.8f, 0.8f);
        const float b = 50.0f + 1500.0f * split(rng);
        const float a = b * split(rng);
        const float whole = std::exp(-AtmosphereMath::opticalDepth(medium, origin, dir, 0.0f, b));
        const float first = std::exp(-AtmosphereMath::opticalDepth(medium, origin, dir, 0.0f, a));
        const float second = std::exp(-AtmosphereMath::opticalDepth(medium, origin, dir, a, b));
        EXPECT_NEAR(first * second, whole, 1e-5f) << "split at " << a << " of " << b;
    }
}

TEST(AtmosphereMath, SegmentCapsTheOpacity){
    FogComponent fog = heightFog();
    fog.attenuationDistance = 1.0f;
    fog.maxOpacity = 0.7f;
    const HeightFogMedium medium = AtmosphereMath::makeMedium(fog);
    const HeightFogSegment segment = AtmosphereMath::segment(medium, Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f), 0.0f, 500.0f,
                                                             Vector3(1.0f, 0.5f, 0.25f));
    EXPECT_NEAR(segment.transmittance, 0.3f, 1e-6f);
    EXPECT_NEAR(segment.scattering.x, 0.7f, 1e-5f);
    EXPECT_NEAR(segment.scattering.y, 0.35f, 1e-5f);
    EXPECT_NEAR(segment.scattering.z, 0.175f, 1e-5f);
}

TEST(AtmosphereMath, PhaseIsNormalisedOverTheSphere){
    for (float g : {-0.6f, 0.0f, 0.3f, 0.8f}){
        // the mean of 4 pi HG over the sphere is 1: integrate over cos theta
        const int steps = 20000;
        double sum = 0.0;
        for (int i = 0; i < steps; i++){
            const float cosTheta = -1.0f + 2.0f * (i + 0.5f) / steps;
            sum += AtmosphereMath::phase(g, cosTheta);
        }
        EXPECT_NEAR(sum / steps, 1.0, 1e-3) << "g " << g;
    }
    EXPECT_FLOAT_EQ(AtmosphereMath::phase(0.0f, 0.3f), 1.0f);
}

TEST(AtmosphereMath, HemiOctahedralRoundTrips){
    std::mt19937 rng(42);
    for (int i = 0; i < 500; i++){
        const Vector3 dir = randomDirection(rng, 0.0f, 1.0f);
        const Vector2 uv = AtmosphereMath::hemiOctahedralEncode(dir);
        EXPECT_GE(uv.x, 0.0f);
        EXPECT_LE(uv.x, 1.0f);
        EXPECT_GE(uv.y, 0.0f);
        EXPECT_LE(uv.y, 1.0f);
        const Vector3 back = AtmosphereMath::hemiOctahedralDecode(uv);
        EXPECT_NEAR((back - dir).length(), 0.0f, 1e-4f) << dir.toString();
    }
    // the zenith is the map's centre
    const Vector2 zenith = AtmosphereMath::hemiOctahedralEncode(Vector3(0.0f, 1.0f, 0.0f));
    EXPECT_NEAR(zenith.x, 0.5f, 1e-6f);
    EXPECT_NEAR(zenith.y, 0.5f, 1e-6f);
}

TEST(AtmosphereMath, SkyFillCoversTheHorizonOnly){
    const float fillSin = std::sin(3.0f * 3.14159265f / 180.0f);
    EXPECT_FLOAT_EQ(AtmosphereMath::skyFillAmount(Vector3(1.0f, -0.2f, 0.0f).normalized(), fillSin, 0.8f), 0.8f);
    EXPECT_FLOAT_EQ(AtmosphereMath::skyFillAmount(Vector3(1.0f, 0.0f, 0.0f), fillSin, 0.8f), 0.8f);
    EXPECT_FLOAT_EQ(AtmosphereMath::skyFillAmount(Vector3(1.0f, 0.5f, 0.0f).normalized(), fillSin, 0.8f), 0.0f);
    EXPECT_FLOAT_EQ(AtmosphereMath::skyFillAmount(Vector3(1.0f, 0.0f, 0.0f), 0.0f, 0.8f), 0.0f);
}

TEST(AtmosphereMath, HorizonRingBlendsByAzimuth){
    const Vector3 ring[4] = {Vector3(1.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f), Vector3(1.0f, 1.0f, 1.0f)};
    EXPECT_TRUE(AtmosphereMath::horizonRingColor(ring, Vector3(0.0f, 0.0f, 1.0f)) == ring[0]);
    EXPECT_TRUE(AtmosphereMath::horizonRingColor(ring, Vector3(1.0f, 0.0f, 0.0f)) == ring[1]);
    const Vector3 between = AtmosphereMath::horizonRingColor(ring, Vector3(1.0f, 0.0f, 1.0f).normalized());
    EXPECT_NEAR(between.x, 0.5f, 1e-5f);
    EXPECT_NEAR(between.y, 0.5f, 1e-5f);
}

TEST(AtmosphereMath, FogVolumeDensityFadesOverTheBlendDistance){
    FogVolumeData box = boxVolume(1, Vector3(0.0f, 0.0f, 0.0f), Vector3(10.0f, 10.0f, 10.0f));
    box.fog.blendDistance = 2.0f;
    EXPECT_FLOAT_EQ(AtmosphereMath::fogVolumeDensity(box, Vector3(0.0f, 0.0f, 0.0f)), 1.0f);
    EXPECT_NEAR(AtmosphereMath::fogVolumeDensity(box, Vector3(4.0f, 0.0f, 0.0f)), 0.5f, 1e-5f);
    EXPECT_FLOAT_EQ(AtmosphereMath::fogVolumeDensity(box, Vector3(5.5f, 0.0f, 0.0f)), 0.0f);
    // 1 m outside the size plus the blend distance: nothing
    EXPECT_FLOAT_EQ(AtmosphereMath::fogVolumeDensity(box, Vector3(8.0f, 0.0f, 0.0f)), 0.0f);

    // rotated 45 degrees about +Y: the box corner direction reaches farther
    FogVolumeData rotated = box;
    rotated.worldToVolume = Quaternion(45.0f, Vector3(0.0f, 1.0f, 0.0f)).getRotationMatrix().inverse();
    EXPECT_GT(AtmosphereMath::fogVolumeDensity(rotated, Vector3(0.0f, 0.0f, 6.0f)), 0.0f);
    EXPECT_FLOAT_EQ(AtmosphereMath::fogVolumeDensity(box, Vector3(0.0f, 0.0f, 6.0f)), 0.0f);

    FogVolumeData sphere = box;
    sphere.fog.shape = FogVolumeShape::SPHERE;
    EXPECT_FLOAT_EQ(AtmosphereMath::fogVolumeDensity(sphere, Vector3(4.5f, 4.5f, 0.0f)), 0.0f);
    EXPECT_GT(AtmosphereMath::fogVolumeDensity(sphere, Vector3(1.0f, 1.0f, 0.0f)), 0.0f);
}

TEST(Atmosphere, VolumeSelection){
    Plane planes[6];
    boxFrustum(planes);
    std::vector<FogVolumeData> volumes;
    // ten volumes in view at rising distances (listed far to near), one behind the camera, one
    // past the volumetric distance
    for (int i = 0; i < 10; i++)
        volumes.push_back(boxVolume((uint32_t)(100 + i), Vector3(0.0f, 0.0f, -10.0f - 6.0f * (9 - i)), Vector3(2.0f, 2.0f, 2.0f)));
    volumes.push_back(boxVolume(200, Vector3(0.0f, 0.0f, 30.0f), Vector3(2.0f, 2.0f, 2.0f)));
    volumes.push_back(boxVolume(201, Vector3(0.0f, 0.0f, -90.0f), Vector3(2.0f, 2.0f, 2.0f)));

    std::vector<int> selected;
    for (int cap : {2, 4, 8}){
        const int left = AtmosphereMath::selectFogVolumes(volumes, planes, 100.0f, Vector3(0.0f, 0.0f, 0.0f), 80.0f, cap, selected);
        ASSERT_EQ((int)selected.size(), cap);
        EXPECT_EQ(left, (int)volumes.size() - cap);
        // the nearest first: the last listed of the ten
        for (int k = 0; k < cap; k++)
            EXPECT_EQ(volumes[selected[k]].entity, (uint32_t)(109 - k)) << "cap " << cap << " slot " << k;
    }
    // without a cap all ten in view and range: neither the one behind nor the far one
    AtmosphereMath::selectFogVolumes(volumes, planes, 100.0f, Vector3(0.0f, 0.0f, 0.0f), 80.0f, 64, selected);
    EXPECT_EQ(selected.size(), 10u);
    for (int index : selected){
        EXPECT_NE(volumes[index].entity, 200u);
        EXPECT_NE(volumes[index].entity, 201u);
    }
    // ties keep the entity order
    std::vector<FogVolumeData> ties = {boxVolume(7, Vector3(5.0f, 0.0f, -20.0f), Vector3(2.0f, 2.0f, 2.0f)),
                                       boxVolume(3, Vector3(-5.0f, 0.0f, -20.0f), Vector3(2.0f, 2.0f, 2.0f))};
    AtmosphereMath::selectFogVolumes(ties, planes, 100.0f, Vector3(0.0f, 0.0f, 0.0f), 80.0f, 2, selected);
    ASSERT_EQ(selected.size(), 2u);
    EXPECT_EQ(ties[selected[0]].entity, 3u);
}
