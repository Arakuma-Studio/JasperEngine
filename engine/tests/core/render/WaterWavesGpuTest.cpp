// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The GPU mirror of the waves (docs/specs/S22-gameplay.md R28, R29, AC11): a test-only post
// shader that includes library/water.glsl writes waterWaves(...).x for a 32 x 32 grid into an
// RGBA32F target at a shader time T, with the xz it evaluated beside it. The readback matches
// engine/core/util/WaterWaves.h at the same xz and T within 1e-3 x waveHeight + 1e-4: what floats
// on the water (Buoyancy, Water.sample) rides the waves the GPU draws.

#include "Engine.h"
#include "pool/ShaderPool.h"
#include "render/CameraRender.h"
#include "render/FramebufferRender.h"
#include "render/ObjectRender.h"
#include "render/PipelineState.h"
#include "render/ShaderRender.h"
#include "render/TextureRender.h"
#include "render/SystemRender.h"
#include "shader/ShaderVariantCompiler.h"
#include "util/WaterWaves.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr int SIZE = 32;

    // r: the height, g and b: the world xz it is at (so the comparison needs no texel orientation),
    // a: a 1 x 1 white texture's 1, so a texel left undrawn shows. The texture also gives the draw
    // a binding: sokol's validation (Debug builds) refuses a draw with no binding at all.
    const char* PROBE = R"(#version 450
#include "library/water.glsl"

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_probeOneTexture;
uniform sampler u_probeOne_smp;

uniform u_fs_waterProbe {
    vec4 wave;      // direction.xy, height, waveLength
    vec4 clock;     // speed, time, extent, unused
} probe;

void main(){
    vec2 xz = (v_texcoord - 0.5) * probe.clock.z;
    vec3 w = waterWaves(xz, probe.clock.y, probe.wave.xy, probe.wave.z, probe.wave.w, probe.clock.x);
    frag_color = vec4(w.x, xz, texture(sampler2D(u_probeOneTexture, u_probeOne_smp), vec2(0.5)).r);
}
)";

    struct ProbeBlock{
        float wave[4];
        float clock[4];
    };

}

namespace {

    // Compiles a probe post shader (source, at the virtual path), draws it once into a SIZE x
    // SIZE RGBA32F target with the block and reads the target back into texels (empty when
    // anything failed: the assertions say why)
    void runProbe(const char* source, const std::string& path, const ProbeBlock& probeBlock, std::vector<float>& texels){
        texels.clear();
        ASSERT_TRUE(ShaderVariantCompiler::isAvailable());
        ASSERT_TRUE(SystemRender::getFeatures().renderRGBA32F) << "the backend renders no RGBA32F target";

        const uint16_t id = ShaderPool::registerCustomShader(path);
        const CustomShaderSources sources = ShaderVariantCompiler::resolveCustomShaderSources(path,
            [&](const std::string& file, std::string& content){
                if (file != path)
                    return false;
                content = source;
                return true;
            }, ShaderType::POSTPROCESS);
        ASSERT_TRUE(sources.valid) << sources.error;
        ShaderData data;
        std::string error, log;
        ASSERT_TRUE(ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::POSTPROCESS, 0, id), ShaderPool::getShaderBackend(),
                                                   &sources, data, &error, &log)) << error << "\n" << log;

        ShaderRender shader;
        ASSERT_TRUE(shader.createShader(data));
        SystemRender::commitQueue();
        SystemRender::executeQueue();
        // created off the render thread (spec S32)
        ASSERT_TRUE(SystemRender::waitGpuCreationIdle(30000));
        ASSERT_TRUE(shader.isCreated());
        const int slot = shader.shaderData.getUniformBlockIndexByName("u_fs_waterProbe");
        ASSERT_GE(slot, 0);
        const std::pair<int, int> oneSlot = shader.shaderData.getTextureIndexByName("u_probeOneTexture");
        ASSERT_GE(oneSlot.first, 0);

        TextureRender one;
        uint8_t white[4] = {255, 255, 255, 255};
        void* oneData[1] = {white};
        size_t oneSize[1] = {sizeof(white)};
        ASSERT_TRUE(one.createTexture("WaterWavesGpu|one", 1, 1, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, oneData, oneSize,
                                      TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE));
        SystemRender::commitQueue();
        SystemRender::executeQueue();
        ASSERT_TRUE(one.isCreated());

        FramebufferRender target;
        FramebufferDesc desc;
        desc.width = SIZE;
        desc.height = SIZE;
        desc.color = ColorFormat::RGBA32F;
        desc.depth = DepthStencilFormat::NONE;
        desc.minFilter = TextureFilter::NEAREST;
        desc.magFilter = TextureFilter::NEAREST;
        desc.label = "WaterWavesGpu";
        ASSERT_TRUE(target.createFramebuffer(desc));

        ObjectRender object;
        object.beginLoad(PrimitiveType::TRIANGLES);
        object.setShader(&shader);
        // a one-off pass: its pipeline is created at once (spec S32), not off the render thread
        object.setImmediatePipelines(true);
        ASSERT_TRUE(object.endLoadDynamic(PrimitiveType::TRIANGLES, IndexType::NONE));
        PipelineState state;
        state.target.color = ColorFormat::RGBA32F;
        state.target.depth = DepthStencilFormat::NONE;
        state.blend = BlendMode::NONE;

        ProbeBlock block = probeBlock;
        CameraRender pass;
        pass.setClearColor(Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        pass.startRenderPass(&target);
        object.addTexture(oneSlot, ShaderStageType::FRAGMENT, &one);
        ASSERT_TRUE(object.beginDraw(state));
        object.applyUniformBlock(slot, sizeof(block), &block);
        object.draw(0, 3, 1);
        pass.endRenderPass();
        SystemRender::commit();

        std::vector<float> read;
        const bool readOk = readTextureFloat(target.getColorTexture(), SIZE, SIZE, read);

        object.destroy();
        shader.destroyShader();
        target.destroyFramebuffer();
        one.destroyTexture();
        SystemRender::commitQueue();
        SystemRender::executeQueue();

        ASSERT_TRUE(readOk);
        ASSERT_EQ(read.size(), (size_t)SIZE * SIZE * 4);
        texels = read;
    }

    // GERSTNER: r the height of the surface point resting at the grid's xz, g and b the world xz
    // the waves moved it to, a 1
    const char* GERSTNER_PROBE = R"(#version 450
#include "library/water.glsl"

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_probeOneTexture;
uniform sampler u_probeOne_smp;

uniform u_fs_waterProbe {
    vec4 wave;      // direction.xy, height, waveLength
    vec4 clock;     // speed, time, extent, steepness
} probe;

void main(){
    vec2 xz = (v_texcoord - 0.5) * probe.clock.z;
    vec3 dPdx;
    vec3 dPdz;
    vec3 offset = waterGerstner(xz, probe.clock.y, probe.wave.xy, probe.wave.z, probe.wave.w, probe.clock.x,
                                probe.clock.w, vec4(1.0), dPdx, dPdz);
    frag_color = vec4(offset.y, xz + offset.xz, texture(sampler2D(u_probeOneTexture, u_probeOne_smp), vec2(0.5)).r);
}
)";

}

TEST(WaterWavesGpu, MatchesTheCpuTwin){
    JASPER_REQUIRE_GPU();
    // a material's waves, at a shader time well into a session, over 40 x 40 world units
    const Vector2 direction(0.8f, -0.45f);
    const float height = 0.35f, waveLength = 6.5f, speed = 2.25f, extent = 40.0f;
    const float time = 1234.5625f;
    const ProbeBlock block = {{direction.x, direction.y, height, waveLength}, {speed, time, extent, 0.0f}};

    std::vector<float> texels;
    runProbe(PROBE, "tests/water_waves_probe.frag", block, texels);
    ASSERT_EQ(texels.size(), (size_t)SIZE * SIZE * 4);

    const float tolerance = 1e-3f * height + 1e-4f;
    float worst = 0.0f;
    for (int i = 0; i < SIZE * SIZE; i++){
        const float gpu = texels[i * 4 + 0];
        const Vector2 xz(texels[i * 4 + 1], texels[i * 4 + 2]);
        ASSERT_EQ(texels[i * 4 + 3], 1.0f) << "texel " << i << " was not drawn";
        const WaterWaveSample cpu = waterWaves(xz, time, direction, height, waveLength, speed);
        worst = std::max(worst, std::fabs(gpu - cpu.height));
        EXPECT_NEAR(gpu, cpu.height, tolerance) << "at xz (" << xz.x << ", " << xz.y << ")";
    }
    std::printf("[gpu] waterWaves GPU vs CPU on %s: worst |dh| %.3g (tolerance %.3g)\n", HeadlessContext::getBackendName(), worst, tolerance);
}

// GERSTNER: the CPU finds, over the world xz the GPU moved a point to, the height the GPU gave it
TEST(WaterWavesGpu, GerstnerMatchesCpu){
    JASPER_REQUIRE_GPU();
    WaterWaveParams params;
    params.waves = true;
    params.gerstner = true;
    params.direction = Vector2(0.8f, -0.45f);
    params.height = 0.35f;
    params.length = 6.5f;
    params.speed = 2.25f;
    params.steepness = 0.7f;
    const float extent = 40.0f;
    const float time = 1234.5625f;
    const ProbeBlock block = {{params.direction.x, params.direction.y, params.height, params.length},
                              {params.speed, time, extent, params.steepness}};

    std::vector<float> texels;
    runProbe(GERSTNER_PROBE, "tests/water_gerstner_probe.frag", block, texels);
    ASSERT_EQ(texels.size(), (size_t)SIZE * SIZE * 4);

    const float tolerance = 1e-3f * params.height + 1e-4f;
    float worst = 0.0f;
    for (int i = 0; i < SIZE * SIZE; i++){
        const float gpu = texels[i * 4 + 0];
        const Vector2 xz(texels[i * 4 + 1], texels[i * 4 + 2]);
        ASSERT_EQ(texels[i * 4 + 3], 1.0f) << "texel " << i << " was not drawn";
        const WaterWaveSample cpu = waterSurfaceAt(params, xz, time);
        worst = std::max(worst, std::fabs(gpu - cpu.height));
        EXPECT_NEAR(gpu, cpu.height, tolerance) << "at xz (" << xz.x << ", " << xz.y << ")";
    }
    std::printf("[gpu] waterGerstner GPU vs CPU on %s: worst |dh| %.3g (tolerance %.3g)\n", HeadlessContext::getBackendName(), worst, tolerance);
}
