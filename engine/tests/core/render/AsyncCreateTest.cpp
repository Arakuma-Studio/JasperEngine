// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Asynchronous shader and pipeline creation (docs/specs/S32-async-shaders.md): pipeline
// interning and its record format, the warm-up list's format (CPU), and on the
// headless device the pending state, deferred destroys, interning, immediate passes, failures,
// every GL mode drawing the LitCube golden, and a stand-in while a variant is pending (GPU).

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Primitive.h"
#include "object/environment/Fog.h"
#include "player/RunControl.h"
#include "pool/ShaderPool.h"
#include "render/ObjectRender.h"
#include "render/ShaderRender.h"
#include "render/SystemRender.h"
#include "shader/RuntimeShaderCompiler.h"
#include "shader/ShaderVariantCompiler.h"
#include "shader/ShaderWarmup.h"
#include "sokol/SokolAsyncCreate.h"
#include "sokol/SokolPipelineCache.h"
#include "sokol/SokolPipelineRecord.h"
#include "subsystem/RenderSystem.h"

#include "json.hpp"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"
#include "TempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    void setEnv(const char* name, const char* value){
    #if defined(_WIN32)
        _putenv_s(name, value ? value : "");
    #else
        if (value)
            setenv(name, value, 1);
        else
            unsetenv(name);
    #endif
    }

    // the creation set up again with JASPER_ASYNC_SHADERS / JASPER_ASYNC_SHADERS_DELAY_MS
    void restartCreation(const char* mode, const char* delayMs = nullptr){
        SokolAsyncCreate::shutdown();
        setEnv("JASPER_ASYNC_SHADERS", mode);
        setEnv("JASPER_ASYNC_SHADERS_DELAY_MS", delayMs);
        SokolAsyncCreate::setup();
    }

    sg_pipeline_desc sampleDesc(){
        sg_pipeline_desc d = {};
        d.shader.id = 7;
        d.layout.buffers[0].stride = 32;
        d.layout.buffers[1].stride = 16;
        d.layout.buffers[1].step_func = SG_VERTEXSTEP_PER_INSTANCE;
        d.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT3;
        d.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT2;
        d.layout.attrs[1].offset = 12;
        d.layout.attrs[2].format = SG_VERTEXFORMAT_UBYTE4N;
        d.layout.attrs[2].buffer_index = 1;
        d.depth.pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL;
        d.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
        d.depth.write_enabled = true;
        d.depth.bias = 1.0f;
        d.stencil.enabled = true;
        d.stencil.front.compare = SG_COMPAREFUNC_EQUAL;
        d.stencil.back.pass_op = SG_STENCILOP_REPLACE;
        d.stencil.read_mask = 0xF0;
        d.stencil.ref = 3;
        d.color_count = 2;
        d.colors[0].pixel_format = SG_PIXELFORMAT_RGBA16F;
        d.colors[0].blend.enabled = true;
        d.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_ONE;
        d.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        d.colors[1].pixel_format = SG_PIXELFORMAT_RGBA8;
        d.colors[1].write_mask = SG_COLORMASK_RGB;
        d.primitive_type = SG_PRIMITIVETYPE_TRIANGLE_STRIP;
        d.index_type = SG_INDEXTYPE_UINT16;
        d.cull_mode = SG_CULLMODE_BACK;
        d.face_winding = SG_FACEWINDING_CCW;
        d.sample_count = 4;
        d.blend_color = {0.1f, 0.2f, 0.3f, 0.4f};
        d.alpha_to_coverage_enabled = true;
        return d;
    }

    // one change of every field interning compares
    std::vector<std::function<void(sg_pipeline_desc&)>> fieldChanges(){
        return {
            [](sg_pipeline_desc& d){ d.shader.id = 8; },
            [](sg_pipeline_desc& d){ d.compute = true; },
            [](sg_pipeline_desc& d){ d.layout.buffers[0].stride = 36; },
            [](sg_pipeline_desc& d){ d.layout.buffers[1].step_func = SG_VERTEXSTEP_PER_VERTEX; },
            [](sg_pipeline_desc& d){ d.layout.buffers[1].step_rate = 2; },
            [](sg_pipeline_desc& d){ d.layout.attrs[1].offset = 16; },
            [](sg_pipeline_desc& d){ d.layout.attrs[2].buffer_index = 0; },
            [](sg_pipeline_desc& d){ d.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT4; },
            [](sg_pipeline_desc& d){ d.depth.pixel_format = SG_PIXELFORMAT_DEPTH; },
            [](sg_pipeline_desc& d){ d.depth.compare = SG_COMPAREFUNC_LESS; },
            [](sg_pipeline_desc& d){ d.depth.write_enabled = false; },
            [](sg_pipeline_desc& d){ d.depth.bias = 2.0f; },
            [](sg_pipeline_desc& d){ d.depth.bias_slope_scale = 1.0f; },
            [](sg_pipeline_desc& d){ d.depth.bias_clamp = 1.0f; },
            [](sg_pipeline_desc& d){ d.stencil.enabled = false; },
            [](sg_pipeline_desc& d){ d.stencil.front.compare = SG_COMPAREFUNC_ALWAYS; },
            [](sg_pipeline_desc& d){ d.stencil.front.fail_op = SG_STENCILOP_ZERO; },
            [](sg_pipeline_desc& d){ d.stencil.back.depth_fail_op = SG_STENCILOP_INVERT; },
            [](sg_pipeline_desc& d){ d.stencil.back.pass_op = SG_STENCILOP_KEEP; },
            [](sg_pipeline_desc& d){ d.stencil.read_mask = 0x0F; },
            [](sg_pipeline_desc& d){ d.stencil.write_mask = 0x01; },
            [](sg_pipeline_desc& d){ d.stencil.ref = 4; },
            [](sg_pipeline_desc& d){ d.color_count = 1; },
            [](sg_pipeline_desc& d){ d.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8; },
            [](sg_pipeline_desc& d){ d.colors[1].write_mask = SG_COLORMASK_RGBA; },
            [](sg_pipeline_desc& d){ d.colors[0].blend.enabled = false; },
            [](sg_pipeline_desc& d){ d.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA; },
            [](sg_pipeline_desc& d){ d.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE; },
            [](sg_pipeline_desc& d){ d.colors[0].blend.op_rgb = SG_BLENDOP_SUBTRACT; },
            [](sg_pipeline_desc& d){ d.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ZERO; },
            [](sg_pipeline_desc& d){ d.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE; },
            [](sg_pipeline_desc& d){ d.colors[0].blend.op_alpha = SG_BLENDOP_MAX; },
            [](sg_pipeline_desc& d){ d.primitive_type = SG_PRIMITIVETYPE_TRIANGLES; },
            [](sg_pipeline_desc& d){ d.index_type = SG_INDEXTYPE_UINT32; },
            [](sg_pipeline_desc& d){ d.cull_mode = SG_CULLMODE_NONE; },
            [](sg_pipeline_desc& d){ d.face_winding = SG_FACEWINDING_CW; },
            [](sg_pipeline_desc& d){ d.sample_count = 1; },
            [](sg_pipeline_desc& d){ d.blend_color.g = 0.5f; },
            [](sg_pipeline_desc& d){ d.alpha_to_coverage_enabled = false; },
        };
    }

}

TEST(PipelineIntern, EveryFieldChangesTheDescription){
    const sg_pipeline_desc base = sampleDesc();
    EXPECT_TRUE(SokolPipelineCache::descEqual(base, base));
    sg_pipeline_desc labelled = base;
    labelled.label = "another label";
    EXPECT_TRUE(SokolPipelineCache::descEqual(base, labelled)) << "the label is not part of the description";
    EXPECT_EQ(SokolPipelineCache::descHash(base), SokolPipelineCache::descHash(labelled));
    size_t index = 0;
    for (const auto& change : fieldChanges()){
        sg_pipeline_desc changed = base;
        change(changed);
        EXPECT_FALSE(SokolPipelineCache::descEqual(base, changed)) << "change " << index;
        EXPECT_NE(SokolPipelineCache::descHash(base), SokolPipelineCache::descHash(changed)) << "change " << index;
        index++;
    }
}

TEST(PipelineRecord, RoundTripsEveryField){
    sg_pipeline_desc base = sampleDesc();
    const std::string text = SokolPipelineRecord::toJson(base);
    sg_pipeline_desc read = {};
    std::string error;
    ASSERT_TRUE(SokolPipelineRecord::fromJson(text, read, &error)) << error;
    // the shader is the list's, never the record's
    EXPECT_EQ(read.shader.id, (uint32_t)SG_INVALID_ID);
    read.shader = base.shader;
    EXPECT_TRUE(SokolPipelineCache::descEqual(base, read)) << text;
    for (const auto& change : fieldChanges()){
        sg_pipeline_desc changed = base;
        change(changed);
        if (changed.compute || changed.shader.id != base.shader.id)
            continue;
        sg_pipeline_desc again = {};
        ASSERT_TRUE(SokolPipelineRecord::fromJson(SokolPipelineRecord::toJson(changed), again, &error)) << error;
        again.shader = changed.shader;
        EXPECT_TRUE(SokolPipelineCache::descEqual(changed, again));
    }
}

TEST(PipelineRecord, RefusesWhatItCannotRead){
    sg_pipeline_desc desc = {};
    std::string error;
    EXPECT_FALSE(SokolPipelineRecord::fromJson("not json", desc, &error));
    EXPECT_FALSE(SokolPipelineRecord::fromJson("[]", desc, &error));
    EXPECT_FALSE(SokolPipelineRecord::fromJson(R"({"primitive":"hexagons"})", desc, &error));
    EXPECT_NE(error.find("hexagons"), std::string::npos) << error;
    EXPECT_FALSE(SokolPipelineRecord::fromJson(R"({"layout":{"attrs":[{"buffer":99,"format":"float3"}]}})", desc, &error));
    EXPECT_FALSE(SokolPipelineRecord::fromJson(R"({"depth":{"write":"yes"}})", desc, &error));
}

TEST(ShaderWarmup, ReadShaderKeysLeavesTheLoadedList){
    const std::string loaded = R"({"version":1,"family":"gl","shaders":[{"key":"0x20000001","custom":""}],"pipelines":[]})";
    std::string error;
    ASSERT_TRUE(ShaderWarmup::loadFromString(loaded, &error)) << error;

    const std::string other = R"({"version":1,"family":"gl","shaders":[{"key":"0x20000002","custom":""},{"key":"0x20000004","custom":""}],"pipelines":[]})";
    std::vector<ShaderKey> keys;
    ASSERT_TRUE(ShaderWarmup::readShaderKeys(other, keys, &error)) << error;
    EXPECT_EQ(keys.size(), 2u);
    EXPECT_EQ(ShaderWarmup::getShaderCount(), 1u) << "the loaded list stays as it was";

    keys.clear();
    EXPECT_FALSE(ShaderWarmup::readShaderKeys("not json", keys, &error));
    EXPECT_TRUE(keys.empty());
    ShaderWarmup::clear();
}

TEST(ShaderWarmup, RoundTrip){
    const std::string record = SokolPipelineRecord::toJson(sampleDesc());
    const std::string list = std::string(R"({"version":1,"shaders":[{"key":"0x1000000ff","custom":""},{"key":"0x200000001","custom":"shaders/myMesh"}],)") +
                             R"("pipelines":[{"shader":1,"family":"gl","desc":)" + record + "}]}";
    std::string error;
    ASSERT_TRUE(ShaderWarmup::loadFromString(list, &error)) << error;
    EXPECT_EQ(ShaderWarmup::getShaderCount(), 2u);
    EXPECT_EQ(ShaderWarmup::getPipelineCount(), 1u);
    EXPECT_EQ(ShaderWarmup::getSkippedCount(), 0u);
    const std::string saved = ShaderWarmup::saveToString();
    ASSERT_TRUE(ShaderWarmup::loadFromString(saved, &error)) << error;
    EXPECT_EQ(ShaderWarmup::saveToString(), saved) << "a saved list reads back the same";
    // merged with itself: nothing new
    ASSERT_TRUE(ShaderWarmup::mergeFromString(saved, &error));
    EXPECT_EQ(ShaderWarmup::getShaderCount(), 2u);
    EXPECT_EQ(ShaderWarmup::getPipelineCount(), 1u);
    ShaderWarmup::clear();
}

TEST(ShaderWarmup, UpgradesThreeAttachmentGBufferRecords){
    // a G-buffer pipeline recorded before the SSR occlusion attachment (colors[3], RGBA8 since it
    // holds the normal's fine bits too)
    sg_pipeline_desc old = {};
    old.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT3;
    old.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
    old.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
    old.depth.write_enabled = true;
    old.color_count = 3;
    for (int i = 0; i < 3; i++)
        old.colors[i].pixel_format = SG_PIXELFORMAT_RGBA8;
    sg_pipeline_desc current = old;
    current.color_count = 4;
    current.colors[3].pixel_format = SG_PIXELFORMAT_RGBA8;

    const std::string gbufferKey = ShaderPool::getShaderKeyText(ShaderPool::getShaderKey(ShaderType::GBUFFER, shaderPropertyBit(1)));
    const std::string meshKey = ShaderPool::getShaderKeyText(ShaderPool::getShaderKey(ShaderType::MESH, shaderPropertyBit(6)));
    const std::string record = SokolPipelineRecord::toJson(old);
    const std::string list = R"({"version":1,"shaders":[{"key":")" + gbufferKey + R"(","custom":""},{"key":")" + meshKey +
                             R"(","custom":""}],"pipelines":[{"shader":0,"family":"gl","desc":)" + record +
                             R"(},{"shader":1,"family":"gl","desc":)" + record + "}]}";
    std::string error;
    ASSERT_TRUE(ShaderWarmup::loadFromString(list, &error)) << error;
    EXPECT_EQ(ShaderWarmup::getPipelineCount(), 2u);
    EXPECT_EQ(ShaderWarmup::getSkippedCount(), 0u);

    const nlohmann::json saved = nlohmann::json::parse(ShaderWarmup::saveToString());
    ASSERT_EQ(saved["pipelines"].size(), 2u);
    // the G-buffer's is the layout the engine asks for now, the other kept as recorded
    sg_pipeline_desc read = {};
    ASSERT_TRUE(SokolPipelineRecord::fromJson(saved["pipelines"][0]["desc"].dump(), read, &error)) << error;
    EXPECT_TRUE(SokolPipelineCache::descEqual(read, current)) << saved["pipelines"][0]["desc"].dump();
    ASSERT_TRUE(SokolPipelineRecord::fromJson(saved["pipelines"][1]["desc"].dump(), read, &error)) << error;
    EXPECT_TRUE(SokolPipelineCache::descEqual(read, old)) << saved["pipelines"][1]["desc"].dump();
    ShaderWarmup::clear();
}

TEST(ShaderWarmup, UpgradesR8OcclusionGBufferRecords){
    // a G-buffer pipeline recorded while colors[3] was the occlusion alone (R8): RGBA8 now (the
    // normal's fine bits), the G-buffer's only; another shader's R8 target is kept
    sg_pipeline_desc old = {};
    old.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT3;
    old.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
    old.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
    old.depth.write_enabled = true;
    old.color_count = 4;
    for (int i = 0; i < 3; i++)
        old.colors[i].pixel_format = SG_PIXELFORMAT_RGBA8;
    old.colors[3].pixel_format = SG_PIXELFORMAT_R8;
    sg_pipeline_desc current = old;
    current.colors[3].pixel_format = SG_PIXELFORMAT_RGBA8;

    const std::string gbufferKey = ShaderPool::getShaderKeyText(ShaderPool::getShaderKey(ShaderType::GBUFFER, shaderPropertyBit(16)));
    const std::string meshKey = ShaderPool::getShaderKeyText(ShaderPool::getShaderKey(ShaderType::MESH, shaderPropertyBit(6)));
    const std::string record = SokolPipelineRecord::toJson(old);
    const std::string list = R"({"version":1,"shaders":[{"key":")" + gbufferKey + R"(","custom":""},{"key":")" + meshKey +
                             R"(","custom":""}],"pipelines":[{"shader":0,"family":"gl","desc":)" + record +
                             R"(},{"shader":1,"family":"gl","desc":)" + record + "}]}";
    std::string error;
    ASSERT_TRUE(ShaderWarmup::loadFromString(list, &error)) << error;
    EXPECT_EQ(ShaderWarmup::getPipelineCount(), 2u);
    EXPECT_EQ(ShaderWarmup::getSkippedCount(), 0u);

    const nlohmann::json saved = nlohmann::json::parse(ShaderWarmup::saveToString());
    ASSERT_EQ(saved["pipelines"].size(), 2u);
    sg_pipeline_desc read = {};
    ASSERT_TRUE(SokolPipelineRecord::fromJson(saved["pipelines"][0]["desc"].dump(), read, &error)) << error;
    EXPECT_TRUE(SokolPipelineCache::descEqual(read, current)) << saved["pipelines"][0]["desc"].dump();
    ASSERT_TRUE(SokolPipelineRecord::fromJson(saved["pipelines"][1]["desc"].dump(), read, &error)) << error;
    EXPECT_TRUE(SokolPipelineCache::descEqual(read, old)) << saved["pipelines"][1]["desc"].dump();
    ShaderWarmup::clear();
}

TEST(ShaderWarmup, SkipsMalformedEntriesAndUnknownVersions){
    std::string error;
    EXPECT_FALSE(ShaderWarmup::loadFromString(R"({"version":2,"shaders":[],"pipelines":[]})", &error));
    EXPECT_NE(error.find("version"), std::string::npos) << error;
    EXPECT_FALSE(ShaderWarmup::loadFromString("[1,2]", &error));
    ASSERT_TRUE(ShaderWarmup::loadFromString(R"({"version":1,"shaders":[{"key":7},{"key":"0x1000000ff"}],)"
                                             R"("pipelines":[{"shader":0,"desc":{}},{"shader":5,"desc":{}},{"shader":1,"desc":"x"},{"shader":1,"desc":{}}]})", &error)) << error;
    EXPECT_EQ(ShaderWarmup::getShaderCount(), 1u);
    EXPECT_EQ(ShaderWarmup::getPipelineCount(), 1u);
    EXPECT_EQ(ShaderWarmup::getSkippedCount(), 4u);
    ShaderWarmup::clear();
}

namespace {

    const char* FLAT = R"(#version 450

in vec2 v_texcoord;
out vec4 frag_color;

void main(){
    frag_color = vec4(v_texcoord, 0.0, 1.0);
}
)";

    class AsyncCreateGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override{
            scene.setBackgroundColor(0.12f, 0.12f, 0.16f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 3.0f, 5.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            Engine::setScene(&scene);
            // what an earlier test left in flight is published before this one counts
            ASSERT_TRUE(SystemRender::waitGpuCreationIdle(30000));
        }

        void TearDown() override{
            Engine::removeScene(&scene);
            // every test leaves the creation as the process started it
            if (std::getenv("JASPER_ASYNC_SHADERS") || std::getenv("JASPER_ASYNC_SHADERS_DELAY_MS"))
                restartCreation(nullptr, nullptr);
        }

        bool render(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }

        // a custom POSTPROCESS program of FLAT, compiled for this backend
        bool compileFlat(const std::string& path, ShaderData& data, std::string& error){
            const uint16_t id = ShaderPool::registerCustomShader(path);
            const CustomShaderSources sources = ShaderVariantCompiler::resolveCustomShaderSources(path,
                [&](const std::string& file, std::string& content){
                    if (file != path)
                        return false;
                    content = FLAT;
                    return true;
                }, ShaderType::POSTPROCESS);
            if (!sources.valid){
                error = sources.error;
                return false;
            }
            std::string log;
            return ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::POSTPROCESS, 0, id), ShaderPool::getShaderBackend(),
                                                  &sources, data, &error, &log);
        }

        std::vector<uint8_t> readTarget(){
            Framebuffer& target = GpuEnvironment::target();
            std::vector<uint8_t> rgba;
            readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
            return rgba;
        }

        static double meanDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
            if (a.size() != b.size() || a.empty())
                return 255.0;
            double sum = 0.0;
            for (size_t i = 0; i + 3 < a.size(); i += 4){
                int largest = 0;
                for (int c = 0; c < 3; c++)
                    largest = std::max(largest, std::abs((int)a[i + c] - (int)b[i + c]));
                sum += largest;
            }
            return sum / (double)(a.size() / 4);
        }
    };

    void litCube(Primitive& cube, Light& sun){
        cube.createBox(1.5f, 1.5f, 1.5f);
        cube.setColor(0.2f, 0.6f, 0.9f);
        cube.setRotation(0.0f, 30.0f, 0.0f);
        sun.setType(LightType::DIRECTIONAL);
        sun.setDirection(-0.4f, -1.0f, -0.6f);
        sun.setIntensity(1.5f);
        sun.setShadows(false);
    }

}

TEST_F(AsyncCreateGpu, AShaderIsPendingUntilItsCreationIsPublished){
    JASPER_REQUIRE_GPU();
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable());
    ShaderData data;
    std::string error;
    ASSERT_TRUE(compileFlat("tests/async_pending.frag", data, error)) << error;

    const GpuCreationStats before = SystemRender::getGpuCreationStats();
    ShaderRender shader;
    ASSERT_TRUE(shader.createShader(data));
    if (SokolAsyncCreate::getMode() != GpuCreationMode::Sync && SokolAsyncCreate::getMode() != GpuCreationMode::Thread){
        // published only by update(): pending however fast the worker is
        EXPECT_TRUE(shader.isPending());
        EXPECT_FALSE(shader.isCreated());
        EXPECT_FALSE(shader.isFailed());
        EXPECT_FALSE(SystemRender::isGpuCreationIdle());
        // a second request while it is pending creates nothing new
        EXPECT_FALSE(shader.createShader(data));
    }
    ASSERT_TRUE(SystemRender::waitGpuCreationIdle(30000));
    EXPECT_TRUE(shader.isCreated());
    EXPECT_FALSE(shader.isPending());
    EXPECT_EQ(SystemRender::getGpuCreationStats().createdShaders, before.createdShaders + 1);
    shader.destroyShader();
    EXPECT_NO_ENGINE_ERRORS();
}

// A strand has asked for a new variant: its MAKE_SHADER is queued, and the main thread gets there
// first, closing a project or reloading its shaders.
namespace {
    template<class MainThread>
    void withAVariantsMakeQueued(uint64_t properties, uint16_t customId, const MainThread& mainThread){
        std::atomic<bool> queued{false};
        std::atomic<bool> letGo{false};
        std::atomic<bool> done{false};
        std::thread strand([&](){
            {
                Engine::AsyncThreadScope scope;
                {
                    std::shared_ptr<ShaderRender> shader = ShaderPool::get(ShaderType::POSTPROCESS, properties, customId);
                    EXPECT_TRUE(shader->loading.load()) << "not queued";
                }
                queued.store(true);
                while (!letGo.load())
                    std::this_thread::yield();
            }
            done.store(true);
        });
        while (!queued.load())
            std::this_thread::yield();
        mainThread();
        letGo.store(true);
        while (!done.load())
            GpuEnvironment::drawFrames(1);
        strand.join();
        GpuEnvironment::drawFrames(2);
        ASSERT_TRUE(SystemRender::waitGpuCreationIdle(30000));
        GpuEnvironment::drawFrames(2);
    }
}

// ShaderPool::clear (a project closing) destroys the handle: its MAKE_SHADER finds it gone. Nothing
// is made, nothing left over, in the slot's next owner either, on each way of creating (sync too).
TEST_F(AsyncCreateGpu, ClearWhileAVariantsMakeIsQueued){
    JASPER_REQUIRE_GPU();
    for (const char* mode : {(const char*)nullptr, "sync"}){
        if (mode)
            restartCreation(mode, nullptr);
        ShaderData data;
        std::string error;
        ASSERT_TRUE(compileFlat("tests/async_queued_clear.frag", data, error)) << error;
        const uint16_t id = ShaderPool::registerCustomShader("tests/async_queued_clear.frag");
        ShaderPool::setShaderBuilder([data](ShaderKey){ return ShaderBuildResult(data, ResourceLoadState::Finished); });
        const uint32_t aliveBefore = sg_query_stats().total.shaders.alive;
        const uint64_t createdBefore = SystemRender::getGpuCreationStats().createdShaders;
        std::shared_ptr<ShaderRender> kept;
        withAVariantsMakeQueued(0, id, [&](){
            kept = ShaderPool::get(ShaderType::POSTPROCESS, 0, id);
            ShaderPool::clear();
            EXPECT_FALSE(kept->isCreated());
        });
        EXPECT_FALSE(kept->isCreated()) << "made after it was destroyed";
        EXPECT_FALSE(kept->isPending());
        EXPECT_FALSE(kept->loading.load()) << "its cleanup ran";
        kept.reset();
        ShaderPool::setShaderBuilder(nullptr);
        EXPECT_LE(sg_query_stats().total.shaders.alive, aliveBefore) << "a GPU shader was left over";
        EXPECT_TRUE(SystemRender::isGpuCreationIdle());
        EXPECT_EQ(SystemRender::getGpuCreationStats().createdShaders, createdBefore) << "the make that found its handle gone is counted";

        // the pool works on: the same source from a new id, and the slot its handle had is not it
        ShaderData again;
        ASSERT_TRUE(compileFlat("tests/async_queued_clear.frag", again, error)) << error;
        const uint16_t newId = ShaderPool::registerCustomShader("tests/async_queued_clear.frag");
        ShaderPool::setShaderBuilder([again](ShaderKey){ return ShaderBuildResult(again, ResourceLoadState::Finished); });
        std::shared_ptr<ShaderRender> shader = ShaderPool::get(ShaderType::POSTPROCESS, 0, newId);
        ASSERT_TRUE(SystemRender::waitGpuCreationIdle(30000));
        EXPECT_TRUE(shader->isCreated());
        shader.reset();
        ShaderPool::remove(ShaderType::POSTPROCESS, 0, newId);
        ShaderPool::setShaderBuilder(nullptr);
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// ShaderPool::destroyCustomShaders (a shader's source edited) means what was asked for before is
// not what draws: a variant still queued, or being created, is built again from the new source.
TEST_F(AsyncCreateGpu, DestroyCustomShadersWhileAVariantsMakeIsQueuedBuildsItAgain){
    JASPER_REQUIRE_GPU();
    ShaderData data;
    std::string error;
    ASSERT_TRUE(compileFlat("tests/async_queued_custom.frag", data, error)) << error;
    const uint16_t id = ShaderPool::registerCustomShader("tests/async_queued_custom.frag");
    std::atomic<int> builds{0};
    ShaderPool::setShaderBuilder([data, &builds](ShaderKey){
        builds.fetch_add(1);
        return ShaderBuildResult(data, ResourceLoadState::Finished);
    });
    withAVariantsMakeQueued(0, id, [](){ ShaderPool::destroyCustomShaders(); });
    EXPECT_EQ(builds.load(), 1);
    std::shared_ptr<ShaderRender> shader = ShaderPool::get(ShaderType::POSTPROCESS, 0, id);
    EXPECT_EQ(builds.load(), 2) << "the edit is not in what draws: the queued make went through";
    ASSERT_TRUE(SystemRender::waitGpuCreationIdle(30000));
    EXPECT_TRUE(shader->isCreated());

    // and one being created on the main thread's side
    shader->destroyShader();
    ASSERT_TRUE(SystemRender::waitGpuCreationIdle(30000));
    builds.store(0);
    shader = ShaderPool::get(ShaderType::POSTPROCESS, 0, id);
    EXPECT_EQ(builds.load(), 1);
    ShaderPool::destroyCustomShaders();
    EXPECT_FALSE(shader->isCreated());
    EXPECT_FALSE(shader->isPending());
    ASSERT_TRUE(SystemRender::waitGpuCreationIdle(30000));
    EXPECT_FALSE(shader->isCreated()) << "made after it was destroyed";
    shader.reset();
    ShaderPool::remove(ShaderType::POSTPROCESS, 0, id);
    ShaderPool::setShaderBuilder(nullptr);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(AsyncCreateGpu, DestroyingAPendingShaderWaitsForItsCreation){
    JASPER_REQUIRE_GPU();
    ShaderData data;
    std::string error;
    ASSERT_TRUE(compileFlat("tests/async_destroy.frag", data, error)) << error;
    ShaderRender shader;
    ASSERT_TRUE(shader.createShader(data));
    const sg_shader handle = shader.backend.get();
    // at once: the worker may still be writing its slot
    shader.destroyShader();
    ASSERT_TRUE(SystemRender::waitGpuCreationIdle(30000));
    EXPECT_EQ(sg_query_shader_state(handle), SG_RESOURCESTATE_INVALID) << "the slot is freed once the creation ended";
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(AsyncCreateGpu, APipelineWaitsForItsShaderAndIsShared){
    JASPER_REQUIRE_GPU();
    ShaderData data;
    std::string error;
    ASSERT_TRUE(compileFlat("tests/async_pipeline.frag", data, error)) << error;
    ShaderRender shader;
    ASSERT_TRUE(shader.createShader(data));

    RenderState state;
    state.depthWrite = false;
    ObjectRender first;
    first.beginLoad(PrimitiveType::TRIANGLES);
    first.setShader(&shader);
    ASSERT_TRUE(first.endLoad((uint16_t)PipelineType::PIP_RTT, state));
    if (shader.isPending()){
        EXPECT_FALSE(first.isReady()) << "a pipeline of a pending shader is pending too";
    }

    ASSERT_TRUE(SystemRender::waitGpuCreationIdle(30000));
    EXPECT_TRUE(first.isReady());

    // the same description: the same pipeline, ready at once (R3)
    ObjectRender second;
    second.beginLoad(PrimitiveType::TRIANGLES);
    second.setShader(&shader);
    ASSERT_TRUE(second.endLoad((uint16_t)PipelineType::PIP_RTT, state));
    EXPECT_EQ(second.getPipelineId(PipelineType::PIP_RTT), first.getPipelineId(PipelineType::PIP_RTT));
    EXPECT_TRUE(second.isReady());

    // another state: another pipeline
    RenderState culled = state;
    culled.faceCulling = true;
    ObjectRender third;
    third.beginLoad(PrimitiveType::TRIANGLES);
    third.setShader(&shader);
    ASSERT_TRUE(third.endLoad((uint16_t)PipelineType::PIP_RTT, culled));
    EXPECT_NE(third.getPipelineId(PipelineType::PIP_RTT), first.getPipelineId(PipelineType::PIP_RTT));

    // the first goes, the shared pipeline stays for the second
    first.destroy();
    EXPECT_TRUE(second.isReady());
    second.destroy();
    third.destroy();
    ASSERT_TRUE(SystemRender::waitGpuCreationIdle(30000));
    shader.destroyShader();
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(AsyncCreateGpu, AnImmediatePassIsCreatedAtOnce){
    JASPER_REQUIRE_GPU();
    ShaderData data;
    std::string error;
    ASSERT_TRUE(compileFlat("tests/async_immediate.frag", data, error)) << error;
    ShaderRender shader;
    ASSERT_TRUE(shader.createShader(data));
    ASSERT_TRUE(SystemRender::waitGpuCreationIdle(30000));

    RenderState state;
    state.depthWrite = false;
    state.blend = BlendMode::ADDITIVE;   // a state no other test made: a new pipeline
    ObjectRender pass;
    pass.beginLoad(PrimitiveType::TRIANGLES);
    pass.setShader(&shader);
    pass.setImmediatePipelines(true);
    ASSERT_TRUE(pass.endLoad((uint16_t)PipelineType::PIP_RTT, state));
    EXPECT_TRUE(pass.isReady()) << "a pass's pipeline is never pending";
    pass.destroy();
    shader.destroyShader();
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(AsyncCreateGpu, AFailedCreationIsFailed){
    JASPER_REQUIRE_GPU();
    // a pipeline of a shader that is gone (a stale handle): FAILED at prepare, never pending
    ShaderData gone;
    std::string goneError;
    ASSERT_TRUE(compileFlat("tests/async_gone.frag", gone, goneError)) << goneError;
    ShaderRender goneShader;
    ASSERT_TRUE(goneShader.createShader(gone));
    ASSERT_TRUE(SystemRender::waitGpuCreationIdle(30000));
    const sg_shader stale = goneShader.backend.get();
    goneShader.destroyShader();
    ASSERT_TRUE(SystemRender::waitGpuCreationIdle(30000));
    sg_pipeline_desc desc = {};
    desc.shader = stale;
    desc.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT3;
    const sg_pipeline pipeline = SokolAsyncCreate::makePipeline(desc);
    EXPECT_FALSE(SokolAsyncCreate::isPending(pipeline));
    EXPECT_NE(sg_query_pipeline_state(pipeline), SG_RESOURCESTATE_VALID);
    SokolAsyncCreate::destroyPipeline(pipeline);
    LogCapture::markTestStart();

#if defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
    // a program the driver refuses (its fragment entry point renamed after the cross-compile)
    ShaderData data;
    std::string error;
    ASSERT_TRUE(compileFlat("tests/async_failed.frag", data, error)) << error;
    for (ShaderStage& stage : data.stages){
        if (stage.type != ShaderStageType::FRAGMENT)
            continue;
        const size_t at = stage.source.find("void main(");
        ASSERT_NE(at, std::string::npos);
        stage.source.replace(at, 10, "void mian(");
    }
    ShaderRender shader;
    shader.createShader(data);
    ASSERT_TRUE(SystemRender::waitGpuCreationIdle(30000));
    EXPECT_TRUE(shader.isFailed());
    EXPECT_FALSE(shader.isCreated());
    EXPECT_FALSE(LogCapture::sinceTestStart().errors().empty()) << "the link failure is reported";
    shader.destroyShader();
#endif
}

#if defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
// every GL mode draws what the synchronous creation drew (the LitCube golden), and says it is the
// mode asked for (llvmpipe lists KHR_parallel_shader_compile; the headless context has a worker)
TEST_F(AsyncCreateGpu, EveryGlModeDrawsTheLitCube){
    JASPER_REQUIRE_GPU();
    scene.setLitQuality(LitQuality::STANDARD);
    for (const char* mode : {"worker", "parallel", "deferred", "sync"}){
        SCOPED_TRACE(mode);
        restartCreation(mode);
        if (std::string(mode) == "worker" && SokolAsyncCreate::isSoftwareGl()){
            // no worker context on a software GL: the driver's compile threads or a deferred status
            EXPECT_NE(SokolAsyncCreate::getMode(), GpuCreationMode::GlWorker);
            EXPECT_NE(SokolAsyncCreate::getMode(), GpuCreationMode::Sync);
        }else{
            EXPECT_STREQ(gpuCreationModeName(SokolAsyncCreate::getMode()), mode);
        }
        const GpuCreationStats before = SystemRender::getGpuCreationStats();
        {
            Primitive cube(&scene);
            Light sun(&scene);
            litCube(cube, sun);
            ASSERT_TRUE(render()) << "the scene never finished loading";
            EXPECT_GOLDEN("render_lit_cube");
            const GpuCreationStats after = SystemRender::getGpuCreationStats();
            EXPECT_GT(after.createdShaders, before.createdShaders) << "the programs were made in this mode";
            if (std::string(mode) == "worker" && SokolAsyncCreate::warmDrawsActive()){
                EXPECT_GT(after.warmDraws, before.warmDraws) << "the worker drew the new programs first";
            }else{
                EXPECT_EQ(after.warmDraws, before.warmDraws) << "no warm draws: off, another mode or a software GL";
            }
        }
        // the cube's programs go, so the next mode makes its own
        GpuEnvironment::drawFrames(2);
        ShaderPool::clearUnused();
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// GL Warm Draws asked for: they run in worker mode, never on a software GL, which has no worker
// context (the render thread crashed drawing programs the worker had drawn on llvmpipe,
// AtmosphereGpu.ScreenCloudsHideFarTerrain, and once hung in a draw)
TEST_F(AsyncCreateGpu, WarmDrawsNeverRunOnASoftwareGl){
    JASPER_REQUIRE_GPU();
    const ShaderCreationSettings kept = SokolAsyncCreate::getSettings();
    ShaderCreationSettings settings = kept;
    settings.glWarmDraws = true;
    SokolAsyncCreate::setSettings(settings);
    restartCreation("worker");
    EXPECT_EQ(SokolAsyncCreate::warmDrawsActive(), !SokolAsyncCreate::isSoftwareGl());
    // nor a worker context there: the platform's choice is another mode
    restartCreation(nullptr);
    if (SokolAsyncCreate::isSoftwareGl()){
        EXPECT_NE(SokolAsyncCreate::getMode(), GpuCreationMode::GlWorker);
    }
    settings.glWarmDraws = false;
    SokolAsyncCreate::setSettings(settings);
    EXPECT_FALSE(SokolAsyncCreate::warmDrawsActive()) << "off when not asked for";
    SokolAsyncCreate::setSettings(kept);
    EXPECT_NO_ENGINE_ERRORS();
}
#endif

// A mesh loading while its variant is held pending draws the nearest ready variant (here without
// the scene's fog), then the exact one; never nothing (R8, AC6)
TEST_F(AsyncCreateGpu, AStandInDrawsWhileTheVariantIsPending){
    JASPER_REQUIRE_GPU();
    scene.setLitQuality(LitQuality::STANDARD);
    auto first = std::make_unique<Primitive>(&scene);
    auto second = std::make_unique<Primitive>(&scene);
    Light sun(&scene);
    litCube(*first, sun);
    first->setPosition(-1.2f, 0.0f, 0.0f);
    second->createBox(1.0f, 1.0f, 1.0f);
    second->setColor(0.9f, 0.4f, 0.2f);
    second->setPosition(1.4f, 0.0f, 0.0f);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> unfogged = readTarget();

    // every creation from now takes 1.5 s on the worker; the second cube loads again into a fog
    second.reset();
    GpuEnvironment::drawFrames(1);
    restartCreation(nullptr, "1500");
    if (SokolAsyncCreate::getMode() == GpuCreationMode::Sync)
        GTEST_SKIP() << "asynchronous creation is off in this run";
    Fog fog(&scene);
    fog.setType(FogType::LINEAR);
    fog.setColor(0.7f, 0.72f, 0.78f);
    fog.setLinearStartEnd(1.0f, 9.0f);
    second = std::make_unique<Primitive>(&scene);
    second->createBox(1.0f, 1.0f, 1.0f);
    second->setColor(0.9f, 0.4f, 0.2f);
    second->setPosition(1.4f, 0.0f, 0.0f);
    GpuEnvironment::drawFrames(3);
    // GL, D3D11, Metal: the fogged programs are pending, so the second cube draws its stand-in.
    // Vulkan creates programs at once and only the pipelines wait: the cube shows up once they
    // are made (a pipeline has no stand-in)
    const bool programsPending = ShaderPool::getPendingShaderCount() > 0;
    ASSERT_TRUE(programsPending || SystemRender::getGpuCreationStats().pendingPipelines > 0) << "the fogged variants are still pending";
    if (programsPending){
        const std::vector<uint8_t> standIn = readTarget();
        EXPECT_LT(meanDifference(standIn, unfogged), 1.0) << "both cubes draw their unfogged variants meanwhile";
    }

    Scene* current = &scene;
    ASSERT_TRUE(GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); }, 3000));
    const std::vector<uint8_t> fogged = readTarget();
    EXPECT_GT(meanDifference(fogged, unfogged), 4.0) << "the exact (fogged) variants replaced the stand-ins";
    EXPECT_GOLDEN("async_standin_fogged");
    EXPECT_NO_ENGINE_ERRORS();
}

// A loaded mesh whose variant changes keeps drawing the one it has until the new variant's
// pipelines are created and warmed, then draws the new one at once: no frame without it, and
// no reload while it waits (a draw waiting for its pipeline is not a failed one, spec S32 R8)
TEST_F(AsyncCreateGpu, AVariantSwitchLeavesNoFrameEmpty){
    JASPER_REQUIRE_GPU();
    scene.setLitQuality(LitQuality::STANDARD);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> background = readTarget();
    Primitive cube(&scene);
    Light sun(&scene);
    litCube(cube, sun);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> unfogged = readTarget();
    const auto covered = [&background](const std::vector<uint8_t>& frame){
        size_t count = 0;
        for (size_t i = 0; i + 3 < frame.size() && i + 3 < background.size(); i += 4){
            int largest = 0;
            for (int c = 0; c < 3; c++)
                largest = std::max(largest, std::abs((int)frame[i + c] - (int)background[i + c]));
            count += largest > 8 ? 1 : 0;
        }
        return count;
    };
    const size_t cubePixels = covered(unfogged);
    ASSERT_GT(cubePixels, unfogged.size() / 4 / 50) << "the cube covers part of the view";

    // every creation from now takes 600 ms off the render thread; fog makes a new variant
    restartCreation(nullptr, "600");
    if (SokolAsyncCreate::getMode() == GpuCreationMode::Sync)
        GTEST_SKIP() << "asynchronous creation is off in this run";
    const uint64_t createdBefore = SystemRender::getGpuCreationStats().createdPipelines;
    Fog fog(&scene);
    fog.setType(FogType::LINEAR);
    fog.setColor(0.7f, 0.72f, 0.78f);
    fog.setLinearStartEnd(1.0f, 9.0f);

    RenderSystem* system = scene.getSystem<RenderSystem>().get();
    int emptyFrames = 0;
    int frames = 0;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(60)){
        GpuEnvironment::drawFrames(1);
        frames++;
        if (covered(readTarget()) < cubePixels / 2)
            emptyFrames++;
        if (frames > 3 && system->isAllLoaded() && SystemRender::isGpuCreationIdle())
            break;
    }
    ASSERT_TRUE(system->isAllLoaded() && SystemRender::isGpuCreationIdle()) << "the variant never came";
    EXPECT_EQ(emptyFrames, 0) << "of " << frames << " frames, some drew no cube";
    // a lit cube's pipelines and the frame's passes, once: none made again frame after frame
    EXPECT_LT(SystemRender::getGpuCreationStats().createdPipelines - createdBefore, 64u);
    ASSERT_TRUE(render());
    EXPECT_GT(meanDifference(readTarget(), unfogged), 4.0) << "the fogged variant draws in the end";
    EXPECT_NO_ENGINE_ERRORS();
}

#if defined(SOKOL_VULKAN)
// the device's pipeline cache is saved, read back when intact, and ignored (then replaced) when it
// is damaged, another device's or cut short (R13, AC11)
TEST_F(AsyncCreateGpu, ThePipelineCacheIsSavedAndValidated){
    JASPER_REQUIRE_GPU();
    {
        Primitive cube(&scene);
        Light sun(&scene);
        litCube(cube, sun);
        ASSERT_TRUE(render());
    }
    SokolAsyncCreate::savePipelineCache();
    const std::string path = SokolAsyncCreate::getPipelineCachePath();
    ASSERT_FALSE(path.empty());
    ASSERT_TRUE(std::filesystem::exists(path));
    const auto readAll = [&path](){
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    };
    const auto writeAll = [&path](const std::string& bytes){
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), (std::streamsize)bytes.size());
    };
    const std::string saved = readAll();
    ASSERT_GT(saved.size(), 64u);
    const auto warningsAbout = [](const char* text){
        return LogCapture::sinceTestStart().count(D_LOG_WARN, text);
    };

    restartCreation("thread");
    EXPECT_EQ(warningsAbout("pipeline cache"), 0u) << "an intact cache is read without a warning";

    std::string damaged = saved;
    damaged[damaged.size() - 1] = (char)(damaged[damaged.size() - 1] ^ 0x5A);
    writeAll(damaged);
    restartCreation("thread");
    EXPECT_EQ(warningsAbout("is damaged"), 1u);
    SokolAsyncCreate::savePipelineCache();
    EXPECT_EQ(readAll().size() >= 64u, true) << "a damaged cache is replaced";

    // another device (its pipelineCacheUUID, the header's bytes 20..35): ignored quietly
    std::string other = readAll();
    other[20] = (char)(other[20] ^ 0x01);
    writeAll(other);
    restartCreation("thread");
    EXPECT_EQ(warningsAbout("is damaged"), 1u);

    writeAll(saved.substr(0, 10));
    restartCreation("thread");
    SokolAsyncCreate::savePipelineCache();
    EXPECT_GT(readAll().size(), 64u) << "a cut-short file is replaced by the next save";
}
#endif

// A variant compiling at runtime is being made too: a screenshot waits for it as it waits for the
// GPU creation (spec S32, S30's --screenshot), or the captured frame has a stand-in where the
// variant goes. Before ShaderPool::isCreationIdle the player's wait read only the GPU creation,
// which is idle while the compile runs, and captured at once.
TEST_F(AsyncCreateGpu, AScreenshotWaitsForAVariantCompilingAtRuntime){
    JASPER_REQUIRE_GPU();
    if (!ShaderVariantCompiler::isAvailable() || !ShaderPool::isRuntimeCompilationEnabled())
        GTEST_SKIP() << "this build compiles no shader at runtime";
    // what the test changes is put back however it ends
    struct Restore {
        bool async = ShaderPool::isAsyncCompilation();
        ~Restore(){
            player::RunControl::reset();
            ShaderPool::setAsyncCompilation(async);
        }
    } restore;
    // on the main thread, one compile a frame: of two variants asked for in one frame, the second
    // compiles at the next frame start (RuntimeShaderCompiler::beginFrame), not before
    ShaderPool::setAsyncCompilation(false);
    // their filter form (Flt): pixelate has no other
    const uint64_t filter = 1;
    const uint16_t first = ShaderPool::registerCustomShader("@effects/pixelate");
    const uint16_t second = ShaderPool::registerCustomShader("@effects/cursor-trail");
    ShaderPool::remove(ShaderType::UI_EFFECT, filter, first);
    ShaderPool::remove(ShaderType::UI_EFFECT, filter, second);
    RuntimeShaderCompiler::shutdown();
    // not from an earlier run's cache either
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(RuntimeShaderCompiler::getCacheDirectory(), ec)){
        const std::string name = entry.path().filename().string();
        if (name.rfind("uieffect_fx_pixelate_", 0) == 0 || name.rfind("uieffect_fx_cursor_trail_", 0) == 0)
            std::filesystem::remove(entry.path(), ec);
    }
    std::shared_ptr<ShaderRender> compiled = ShaderPool::get(ShaderType::UI_EFFECT, filter, first);
    std::shared_ptr<ShaderRender> waiting = ShaderPool::get(ShaderType::UI_EFFECT, filter, second);
    if (waiting->isCreated())
        GTEST_SKIP() << "this build ships the effect precompiled";
    ASSERT_GT(RuntimeShaderCompiler::getPendingCount(), 0u);
    ASSERT_TRUE(SystemRender::waitGpuCreationIdle(30000));
    EXPECT_TRUE(SystemRender::isGpuCreationIdle()) << "nothing is being created on the GPU meanwhile";
    EXPECT_FALSE(ShaderPool::isCreationIdle()) << "the compile is counted";

    test::TempDir dir;
    player::LaunchOptions options;
    options.screenshotFrame = 1;
    options.screenshot = dir.path() / "compiled.png";
    // the player's own check (no probe): ShaderPool::isCreationIdle
    player::RunControl::configure(options);
    int reads = 0;
    const player::FrameReader reader = [&reads](std::vector<uint8_t>& rgba, int& width, int& height, std::string&){
        reads++;
        width = 1;
        height = 1;
        rgba = {0, 0, 0, 255};
        return true;
    };
    player::RunControl::frameDrawn(true, reader);
    EXPECT_EQ(reads, 0) << "the frame without the variant is not captured";

    // the next frames compile it, create it and then capture
    for (int frame = 0; frame < 600 && reads == 0; frame++){
        RuntimeShaderCompiler::beginFrame();
        waiting = ShaderPool::get(ShaderType::UI_EFFECT, filter, second);
        SystemRender::updateGpuCreation();
        if (!waiting->isCreated()){
            EXPECT_FALSE(ShaderPool::isCreationIdle()) << "frame " << frame << ": not created yet, and not counted";
        }
        player::RunControl::frameDrawn(false, reader);
        if (reads == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(reads, 1);
    EXPECT_TRUE(waiting->isCreated()) << "captured once the variant is made";
    EXPECT_TRUE(compiled->isCreated());
    EXPECT_EQ(player::RunControl::finish(), player::EXIT_OK);
    EXPECT_NO_ENGINE_ERRORS();
}
