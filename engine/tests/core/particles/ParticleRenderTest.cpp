// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The 32-byte instances: what they hold, the back-to-front sort, the mesh groups, the half
// conversion, and the native packing against the scalar one.

#include "ParticleTestEffects.h"
#include "ParticleTestUtil.h"

#include "memory/Arena.h"
#include "particles/ParticleRender.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <random>

using namespace particletest;

TEST(ParticleRender, HalfConversion){
    for (float v : {0.0f, 1.0f, -2.5f, 0.333f, 65504.0f, 1.0e-5f, 6.1e-5f, 100.0f}){
        const uint16_t h = ParticleRenderPrep::toHalf(v);
        EXPECT_NEAR(ParticleRenderPrep::fromHalf(h), v, std::fabs(v) * 1e-3f + 1e-7f) << v;
    }
    // rounding to nearest even, overflow to infinity
    EXPECT_EQ(ParticleRenderPrep::toHalf(1.0f), 0x3c00u);
    EXPECT_EQ(ParticleRenderPrep::toHalf(-2.0f), 0xc000u);
    EXPECT_EQ(ParticleRenderPrep::toHalf(1.0e6f), 0x7c00u);
    EXPECT_EQ(ParticleRenderPrep::toHalf(0.5f + 1.0f / 4096.0f), 0x3800u);
}

TEST(ParticleRender, InstanceHoldsTheParticle){
    ParticleEffectDocument d = singleParticle(Vector3(1.0f, 2.0f, 3.0f));
    d.main.startSize = MinMaxCurve(0.75f);
    d.main.startRotation = MinMaxCurve(90.0f);
    d.main.startColor = MinMaxGradient(Vector4(1.0f, 0.5f, 0.0f, 0.25f));
    d.renderer.emissionIntensity = 3.0f;
    d.interpolate = false;
    Sim sim(d);
    sim.ticks(2);
    memory::Arena arena("ParticleRenderTest");
    const PackResult pack = ParticleRenderPrep::pack(sim.runtime, RenderView(), arena);
    ASSERT_EQ(pack.count, 1u);
    const ParticleInstance& p = pack.instances[0];
    EXPECT_FLOAT_EQ(p.position[0], sim.ch(ChPosX));
    EXPECT_FLOAT_EQ(p.position[1], sim.ch(ChPosY));
    EXPECT_NEAR(ParticleRenderPrep::fromHalf((uint16_t)(p.size & 0xffffu)), 0.75f, 1e-3f);
    EXPECT_NEAR((float)(p.rotationFrame & 0xffffu) / 65535.0f, 0.25f, 1e-4f);
    // sRGB-encoded rgb, linear alpha
    EXPECT_EQ(p.color & 255u, 255u);
    EXPECT_NEAR((float)((p.color >> 8) & 255u), 127.5f, 1.0f);
    EXPECT_EQ((p.color >> 16) & 255u, 0u);
    EXPECT_NEAR((float)(p.color >> 24), 0.25f * 255.0f, 1.0f);
    EXPECT_NEAR(ParticleRenderPrep::fromHalf((uint16_t)(p.velocityXY & 0xffffu)), 1.0f, 1e-3f);
    EXPECT_NEAR(ParticleRenderPrep::fromHalf((uint16_t)(p.velocityZW >> 16)), 3.0f, 1e-3f);
}

TEST(ParticleRender, SortsBackToFront){
    ParticleEffectDocument d;
    d.tickRate = 30;
    d.maxParticles = 500;
    d.main.startLifetime = MinMaxCurve(100.0f);
    d.main.startSpeed = MinMaxCurve(0.0f);
    d.shape.shape = ShapeType::Box;
    d.shape.scale = Vector3(20.0f, 20.0f, 20.0f);
    d.emission.rateOverTime = MinMaxCurve(0.0f);
    Burst b;
    b.count = MinMaxCurve(500.0f);
    d.emission.bursts = {b};
    d.renderer.blend = ParticleBlend::Alpha;
    d.renderer.sortMode = SortMode::ByDistance;
    Sim sim(d);
    sim.ticks(1);
    memory::Arena arena("ParticleRenderTest");
    RenderView view;
    view.position[2] = 30.0f;
    view.sort = true;
    const PackResult pack = ParticleRenderPrep::pack(sim.runtime, view, arena);
    ASSERT_EQ(pack.count, 500u);
    // 16-bit keys over the bounds' range: far first, within a key's width
    const float step = 70.0f / 65535.0f * 2.0f;
    float previous = 1e9f;
    for (uint32_t i = 0; i < pack.count; i++){
        const float* p = pack.instances[i].position;
        const float dist = std::sqrt(p[0] * p[0] + p[1] * p[1] + (p[2] - 30.0f) * (p[2] - 30.0f));
        EXPECT_LE(dist, previous + step) << i;
        previous = dist;
    }
}

TEST(ParticleRender, RadixSortIsStable){
    std::mt19937 rng(3);
    std::vector<uint16_t> keys(5000);
    for (auto& k : keys) k = (uint16_t)(rng() % 300);
    std::vector<uint32_t> order(keys.size()), scratch(keys.size());
    for (uint32_t i = 0; i < order.size(); i++) order[i] = i;
    ParticleRenderPrep::radixSort16(keys.data(), order.data(), scratch.data(), (uint32_t)keys.size());
    for (size_t i = 1; i < order.size(); i++){
        ASSERT_LE(keys[order[i - 1]], keys[order[i]]);
        if (keys[order[i - 1]] == keys[order[i]]){
            ASSERT_LT(order[i - 1], order[i]);
        }
    }
}

TEST(ParticleRender, MeshesGetTheirOwnGroups){
    ParticleEffectDocument d;
    d.maxParticles = 1000;
    d.main.startLifetime = MinMaxCurve(100.0f);
    d.emission.rateOverTime = MinMaxCurve(0.0f);
    Burst b;
    b.count = MinMaxCurve(1000.0f);
    d.emission.bursts = {b};
    d.renderer.renderMode = RenderMode::Mesh;
    MeshSource cube, sphere, quad;
    sphere.primitive = PrimitiveMesh::Sphere;
    quad.primitive = PrimitiveMesh::Quad;
    d.renderer.meshes = {cube, sphere, quad};
    d.renderer.meshSelection = MeshSelection::Index;
    Sim sim(d);
    sim.ticks(1);
    memory::Arena arena("ParticleRenderTest");
    const PackResult pack = ParticleRenderPrep::pack(sim.runtime, RenderView(), arena);
    ASSERT_EQ(pack.count, 1000u);
    ASSERT_NE(pack.mesh, nullptr);
    // spawn index modulo 3
    EXPECT_EQ(pack.meshCount[0], 334u);
    EXPECT_EQ(pack.meshCount[1], 333u);
    EXPECT_EQ(pack.meshCount[2], 333u);
    EXPECT_EQ(pack.meshFirst[1], 334u);
    EXPECT_EQ(pack.meshCount[3], 0u);
    // a unit quaternion
    const int16_t* q = pack.mesh[0].rotation;
    const float len = std::sqrt((float)q[0] * q[0] + (float)q[1] * q[1] + (float)q[2] * q[2] + (float)q[3] * q[3]) / 32767.0f;
    EXPECT_NEAR(len, 1.0f, 1e-3f);
}

// the native packing and the scalar one give the same bytes
TEST(ParticleRender, NativeMatchesScalar){
    ParticleEffectDocument d = referenceStack(true, 20000.0f, 16384);
    d.colorBySpeed.present = true;
    d.colorBySpeed.color.mode = GradientValueMode::Gradient;
    d.colorBySpeed.color.gradient = Gradient::twoColors(Vector3(1, 1, 1), Vector3(0.2f, 0.4f, 1.0f));
    d.colorBySpeed.speedRange = Vector2(0.0f, 5.0f);
    d.rotationOverLifetime.present = true;
    d.rotationOverLifetime.angularVelocity = MinMaxCurve::twoConstants(-90.0f, 90.0f);
    d.renderer.sortMode = SortMode::ByDistance;
    d.renderer.blend = ParticleBlend::Alpha;
    Sim sim(d);
    sim.ticks(40);
    ASSERT_GT(sim.count(), 10000u);
    memory::Arena a("ParticleRenderTest.native"), b("ParticleRenderTest.scalar");
    RenderView view;
    view.position[1] = 2.0f;
    view.position[2] = 8.0f;
    view.sort = true;
    view.keep = 0.7f;
    const PackResult native = ParticleRenderPrep::packWith(false, sim.runtime, view, a, nullptr);
    const PackResult scalar = ParticleRenderPrep::packWith(true, sim.runtime, view, b, nullptr);
    ASSERT_EQ(native.count, scalar.count);
    EXPECT_EQ(std::memcmp(native.instances, scalar.instances, native.count * sizeof(ParticleInstance)), 0);
}
