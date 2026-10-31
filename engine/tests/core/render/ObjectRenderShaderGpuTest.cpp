// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// A mesh's pipelines outlive the shader they were built on when the shader goes before the mesh
// loads again (a shader reloaded or released while the mesh keeps its old pipelines for a frame:
// the MCP test_autosync fork-then-screenshot crash). sokol keeps such a pipeline valid; applying
// it and its bindings read the dead shader (a SIGSEGV in sg_apply_bindings in Release). The
// object refuses to draw instead (SokolObject::beginDraw), until a reload builds new pipelines.

#include "pool/ShaderPool.h"
#include "render/ObjectRender.h"
#include "render/ShaderRender.h"
#include "render/SystemRender.h"
#include "shader/ShaderVariantCompiler.h"

#include "GpuEnvironment.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const char* FLAT = R"(#version 450

in vec2 v_texcoord;
out vec4 frag_color;

void main(){
    frag_color = vec4(v_texcoord, 0.0, 1.0);
}
)";

}

TEST(ObjectRenderShaderGpu, APipelineOfADeadShaderIsNeverApplied){
    JASPER_REQUIRE_GPU();
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable());

    const std::string path = "tests/object_render_dead_shader.frag";
    const uint16_t id = ShaderPool::registerCustomShader(path);
    const CustomShaderSources sources = ShaderVariantCompiler::resolveCustomShaderSources(path,
        [&](const std::string& file, std::string& content){
            if (file != path)
                return false;
            content = FLAT;
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

    ObjectRender object;
    object.beginLoad(PrimitiveType::TRIANGLES);
    object.setShader(&shader);
    RenderState state;
    state.depthWrite = false;
    ASSERT_TRUE(object.endLoad((uint16_t)PipelineType::PIP_RTT, state));
    SystemRender::commitQueue();
    SystemRender::executeQueue();
    ASSERT_TRUE(SystemRender::waitGpuCreationIdle(30000));

    // the shader goes, the object keeps its pipelines (it has not loaded again)
    shader.destroyShader();
    SystemRender::commitQueue();
    SystemRender::executeQueue();
    EXPECT_FALSE(object.beginDraw(PipelineType::PIP_RTT)) << "a pipeline whose shader is gone is not applied";
    EXPECT_FALSE(object.beginDraw(PipelineType::PIP_RTT, true)) << "nor its no-culling variant";

    object.destroy();
    SystemRender::commitQueue();
    SystemRender::executeQueue();
}
