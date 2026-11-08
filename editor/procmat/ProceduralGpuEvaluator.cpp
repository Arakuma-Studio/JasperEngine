// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProceduralGpuEvaluator.h"

#include "pool/ShaderPool.h"
#include "render/CameraRender.h"
#include "render/FrameReadback.h"
#include "render/ObjectRender.h"
#include "render/PipelineState.h"
#include "render/ShaderRender.h"
#include "render/SystemRender.h"
#include "render/TextureRender.h"
#include "shader/ShaderVariantCompiler.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor::procmat;

namespace {

    // A fullscreen triangle with no vertex buffer. Row 0 of every target holds v near 0 on every
    // backend, whichever way up its framebuffer is, so a read back and a later sample agree.
    const char* FULLSCREEN_VERTEX = R"(#version 450
out vec2 v_uv;
void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    #ifndef IS_GLSL
        v_uv.y = 1.0 - v_uv.y;
    #endif
    #ifdef IS_VULKAN
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
)";

    // Read back is RGBA8 only, and each backend converts out-of-range floats its own way: clamped here (R12)
    const char* QUANTIZE_FRAGMENT = R"(#version 450
in vec2 v_uv;
out vec4 g_finalColor;
uniform texture2D t_source;
uniform sampler s_source;
void main() {
    g_finalColor = clamp(textureLod(sampler2D(t_source, s_source), v_uv, 0.0), 0.0, 1.0);
}
)";

    // One level of a box mip chain: a bilinear tap on the corner four texels share, at level `p.x`
    const char* MIP_BOX_FRAGMENT = R"(#version 450
in vec2 v_uv;
out vec4 g_finalColor;
uniform texture2D t_source;
uniform sampler s_source;
uniform u_fs_procmat {
    vec4 p;
} pm;
void main() {
    g_finalColor = textureLod(sampler2D(t_source, s_source), v_uv, pm.p.x);
}
)";

    // sokol's frame budgets drop a draw silently when they run out: a slice leaves room for the
    // editor's own frame and continues on the next
    constexpr int MAX_DRAWS_PER_FRAME = 128;

    int mipLevels(int size) {
        int levels = 1;
        while ((size >> levels) > 0) levels++;
        return levels;
    }

    // A pass's object, once its shader exists: creation may run off this thread
    bool loadObject(std::unique_ptr<ObjectRender>& object, ShaderRender& shader) {
        object = std::make_unique<ObjectRender>();
        object->beginLoad(PrimitiveType::TRIANGLES);
        object->setShader(&shader);
        // a pass of its own: nothing else draws until it has, so it is never skipped waiting for a warm draw
        object->setImmediatePipelines(true);
        return object->endLoadDynamic(PrimitiveType::TRIANGLES, IndexType::NONE);
    }

    std::unique_ptr<TextureRender> makeTarget(int size, ColorFormat format, int levels) {
        auto texture = std::make_unique<TextureRender>();
        const TextureFilter minification = levels > 1 ? TextureFilter::LINEAR_MIPMAP_LINEAR : TextureFilter::LINEAR;
        if (!texture->createFramebufferTexture(TextureType::TEXTURE_2D, false, false, size, size, minification, TextureFilter::LINEAR,
                                               TextureWrap::REPEAT, TextureWrap::REPEAT, format, 1, false, false, levels)) {
            return nullptr;
        }
        return texture;
    }

}

struct ProceduralGpuEvaluator::Program {
    std::shared_ptr<ShaderRender> shader;
    std::unique_ptr<ObjectRender> object;
    bool loaded = false;
    // kept with the program so a failure is reported once with its log
    std::string log;

    ~Program() {
        if (object) object->destroy();
        if (shader) shader->destroyShader();
    }
};

struct ProceduralGpuEvaluator::Target {
    std::unique_ptr<TextureRender> texture;
    // the other half of the mip chain: a pass never samples the texture it draws into
    std::unique_ptr<TextureRender> reduce;
    int size = 0;
    bool mips = false;
};

struct ProceduralGpuEvaluator::Buffer {
    std::vector<std::unique_ptr<Target>> targets;
    Target* front = nullptr;
};

ProceduralGpuEvaluator::ProceduralGpuEvaluator(EvaluatorTier tier) : tierName(tier) {}

ProceduralGpuEvaluator::~ProceduralGpuEvaluator() {
    for (const std::unique_ptr<Buffer>& buffer : buffers) {
        for (const std::unique_ptr<Target>& target : buffer->targets) {
            target->texture->destroyTexture();
            if (target->reduce) target->reduce->destroyTexture();
        }
    }
    for (auto& [name, texture] : outputs) texture->destroyTexture();
}

std::string ProceduralGpuEvaluator::deviceName() const {
    return SystemRender::getDeviceName();
}

bool ProceduralGpuEvaluator::prepare(const PassPlan& newPlan, std::string& error) {
    if (!SystemRender::getFeatures().renderRGBA16F) {
        error = "this device cannot render into RGBA16F buffers";
        return false;
    }
    if (!ShaderVariantCompiler::isAvailable()) {
        error = "this build has no shader compiler";
        return false;
    }
    injectedCompileFailure.clear();
    injectedReadBackFailure = false;
    if (const char* injected = std::getenv("JASPER_TEST_PROCMAT_DEVICE_FAIL")) {
        const std::string what = injected;
        if (what.rfind("compile:", 0) == 0) injectedCompileFailure = what.substr(8);
        injectedReadBackFailure = what == "readback";
    }
    plan = newPlan;
    // programs still used stay; the others go now, since the sokol pools hold 2048 of everything (R24)
    std::map<std::string, std::unique_ptr<Program>> kept;
    for (const PlannedPass& pass : plan.passes) {
        auto it = programs.find(pass.fragment);
        if (it != programs.end()) kept[pass.fragment] = std::move(it->second);
    }
    programs = std::move(kept);
    passPrograms.assign(plan.passes.size(), nullptr);
    passTextureSlots.assign(plan.passes.size(), {});

    // buffers are kept by position when they still fit: most edits leave most buffers alone
    while (buffers.size() > plan.buffers.size()) {
        for (const std::unique_ptr<Target>& target : buffers.back()->targets) {
            target->texture->destroyTexture();
            if (target->reduce) target->reduce->destroyTexture();
        }
        buffers.pop_back();
    }
    while (buffers.size() < plan.buffers.size()) buffers.push_back(std::make_unique<Buffer>());
    for (const std::unique_ptr<Buffer>& buffer : buffers) buffer->front = nullptr;
    dropUnusedTargets();

    nextPass = 0;
    nextStep = 0;
    nextOutput = 0;
    phase = Phase::Compile;
    return true;
}

void ProceduralGpuEvaluator::dropUnusedTargets() {
    for (size_t index = 0; index < buffers.size(); index++) {
        std::vector<std::unique_ptr<Target>>& targets = buffers[index]->targets;
        const bool mips = plan.buffers[index].mips;
        const auto unused = [&](const std::unique_ptr<Target>& target) {
            const bool used = target->mips == mips && std::any_of(plan.steps.begin(), plan.steps.end(), [&](const PlannedStep& step) {
                return step.buffer == (int)index && step.size == target->size;
            });
            if (used) return false;
            target->texture->destroyTexture();
            if (target->reduce) target->reduce->destroyTexture();
            return true;
        };
        targets.erase(std::remove_if(targets.begin(), targets.end(), unused), targets.end());
    }
}

void ProceduralGpuEvaluator::setUniforms(uint32_t pass, const UniformBlock& block) {
    if (pass >= plan.passes.size()) return;
    plan.passes[pass].uniforms = block;
    const size_t first = firstStepToRedraw(plan, pass);
    if (first == plan.steps.size()) return;
    // what was drawn before it stays, and a cancelled evaluation may not have got that far
    nextStep = std::min(nextStep, first);
    nextOutput = 0;
    phase = nextPass < plan.passes.size() ? Phase::Compile : Phase::Draw;
}

void ProceduralGpuEvaluator::cancel() {
    phase = Phase::Idle;
}

void ProceduralGpuEvaluator::fail(EvaluationResult& result, const std::string& error, int pass, const std::string& log) {
    result = EvaluationResult();
    result.error = error;
    result.failedPass = pass;
    result.log = log;
    phase = Phase::Idle;
}

bool ProceduralGpuEvaluator::createProgram(Program& program, const std::string& fragment, const std::string& label, EvaluationResult& result) {
    ShaderData data;
    std::string error;
    compiles++;
    if (!ShaderVariantCompiler::compileSources(FULLSCREEN_VERTEX, fragment, ShaderPool::getShaderBackend(), data, &error, &program.log)) {
        fail(result, label + ": " + error, -1, program.log);
        return false;
    }
    program.shader = std::make_shared<ShaderRender>();
    if (!program.shader->createShader(data)) {
        fail(result, label + ": the device refused the shader", -1, program.log);
        return false;
    }
    return true;
}

bool ProceduralGpuEvaluator::compilePass(size_t index, EvaluationResult& result) {
    const PlannedPass& pass = plan.passes[index];
    if (!injectedCompileFailure.empty() && pass.label == injectedCompileFailure) {
        fail(result, pass.label + ": Error compiling the shader", (int)index, "ERROR: 0:1: injected by JASPER_TEST_PROCMAT_DEVICE_FAIL");
        return false;
    }
    std::unique_ptr<Program>& program = programs[pass.fragment];
    if (!program) {
        program = std::make_unique<Program>();
        if (!createProgram(*program, pass.fragment, pass.label, result)) {
            result.failedPass = (int)index;
            programs.erase(pass.fragment);
            return false;
        }
    }
    passPrograms[index] = program.get();
    return true;
}

ProceduralGpuEvaluator::Target* ProceduralGpuEvaluator::acquireTarget(Buffer& buffer, int size, bool mips, bool sampledByItself) {
    for (const std::unique_ptr<Target>& target : buffer.targets) {
        if (target->size == size && target->mips == mips && (!sampledByItself || target.get() != buffer.front)) return target.get();
    }
    auto target = std::make_unique<Target>();
    target->size = size;
    target->mips = mips;
    const int levels = mips ? mipLevels(size) : 1;
    target->texture = makeTarget(size, ColorFormat::RGBA16F, levels);
    if (!target->texture) return nullptr;
    if (mips) {
        target->reduce = makeTarget(size, ColorFormat::RGBA16F, levels);
        if (!target->reduce) return nullptr;
    }
    buffer.targets.push_back(std::move(target));
    return buffer.targets.back().get();
}

bool ProceduralGpuEvaluator::frameHasRoom(size_t uniformBytes) {
    const uint64_t frame = SystemRender::getFrameCount();
    if (frame != budgetFrame) {
        budgetFrame = frame;
        frameUniformBytes = 0;
        frameDraws = 0;
    }
    if (frameDraws >= MAX_DRAWS_PER_FRAME) return false;
    if (SystemRender::hasUniformBufferLimit() && frameUniformBytes + uniformBytes > SystemRender::getUniformBytesLimit() / 2) return false;
    frameDraws++;
    frameUniformBytes += uniformBytes;
    return true;
}

bool ProceduralGpuEvaluator::drawInto(Program& program, TextureRender& target, int mip, const void* uniforms, size_t uniformBytes,
                                      const std::vector<std::pair<std::pair<int, int>, TextureRender*>>& textures, bool floatTarget) {
    PipelineState state;
    state.target.color = floatTarget ? ColorFormat::RGBA16F : ColorFormat::RGBA;
    state.target.depth = DepthStencilFormat::NONE;
    state.target.sampleCount = 1;
    state.blend = BlendMode::NONE;

    CameraRender pass;
    // every texel is written: nothing to load
    pass.setColorActions(LoadAction::DONTCARE, StoreAction::STORE);
    pass.setLabel("procedural material");
    pass.startRenderPass(&target, 0, mip);
    for (const auto& [slot, texture] : textures) program.object->addTexture(slot, ShaderStageType::FRAGMENT, texture);
    const bool ready = program.object->beginDraw(state);
    if (ready) {
        if (uniformBytes > 0) {
            const int slot = program.shader->shaderData.getUniformBlockIndexByName("u_fs_procmat");
            if (slot >= 0) program.object->applyUniformBlock(slot, (unsigned int)uniformBytes, const_cast<void*>(uniforms));
        }
        program.object->draw(0, 3, 1);
    }
    pass.endRenderPass();
    return ready;
}

bool ProceduralGpuEvaluator::drawStep(size_t index, EvaluationResult& result, bool& failed) {
    const PlannedStep& step = plan.steps[index];
    Buffer& buffer = *buffers[(size_t)step.buffer];
    const bool mips = plan.buffers[(size_t)step.buffer].mips;
    if (step.pass < 0) {
        // a Buffer with nothing wired: a clear, drawn like a pass so its target exists
        Target* target = acquireTarget(buffer, step.size, mips, false);
        if (!target) {
            failed = true;
            fail(result, "cannot make a buffer of " + std::to_string(step.size), -1, {});
            return true;
        }
        CameraRender pass;
        pass.setColorActions(LoadAction::CLEAR, StoreAction::STORE, Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        pass.setLabel("procedural material");
        pass.startRenderPass(target->texture.get(), 0, 0);
        pass.endRenderPass();
        buffer.front = target;
        // its levels are read like a drawn Buffer's, and were never written
        return mips ? generateMips(*target, result, failed) : true;
    }

    const PlannedPass& planned = plan.passes[(size_t)step.pass];
    Program& program = *passPrograms[(size_t)step.pass];
    if (program.shader->isPending()) return false;
    if (!program.shader->isCreated()) {
        failed = true;
        fail(result, planned.label + ": the device failed to create its shader", step.pass, program.log);
        return true;
    }
    if (!program.object) {
        if (!loadObject(program.object, *program.shader)) {
            failed = true;
            fail(result, planned.label + ": the device refused its pipeline", step.pass, program.log);
            return true;
        }
    }
    std::vector<std::pair<int, int>>& slots = passTextureSlots[(size_t)step.pass];
    if (slots.size() != planned.textures.size()) {
        slots.clear();
        // a texture a pass names only for its size is optimized out of the shader: nothing to bind
        for (const PlannedTexture& texture : planned.textures) slots.push_back(program.shader->shaderData.getTextureIndexByName("t_" + texture.name));
    }

    // std140 vec4s: a block whose floats do not fill its last vec4 is padded
    std::vector<float> block = planned.uniforms.values;
    block.resize((block.size() + 3) / 4 * 4, 0.0f);
    const size_t bytes = block.size() * sizeof(float);
    if (!frameHasRoom(bytes)) return false;

    // the size each Buffer read here has now: a shrinking Iterate Buffer's is not the one generated
    for (const PlannedSize& size : planned.sizes) {
        const Target* front = buffers[(size_t)size.buffer]->front;
        if (front && size.index < block.size()) block[size.index] = (float)front->size;
    }

    bool sampledByItself = false;
    std::vector<std::pair<std::pair<int, int>, TextureRender*>> textures;
    for (size_t i = 0; i < planned.textures.size(); i++) {
        if (slots[i].first < 0) continue;
        const int source = planned.textures[i].buffer;
        sampledByItself = sampledByItself || source == step.buffer;
        Target* front = buffers[(size_t)source]->front;
        if (!front) {
            failed = true;
            fail(result, planned.label + " reads a buffer that was never drawn", step.pass, {});
            return true;
        }
        textures.push_back({slots[i], front->texture.get()});
    }
    Target* target = acquireTarget(buffer, step.size, mips, sampledByItself);
    if (!target) {
        failed = true;
        fail(result, "cannot make a buffer of " + std::to_string(step.size), step.pass, {});
        return true;
    }
    if (!drawInto(program, *target->texture, 0, block.data(), bytes, textures, true)) {
        if (program.object->isDrawWaiting()) return false;
        failed = true;
        fail(result, planned.label + ": the device has no pipeline for it", step.pass, program.log);
        return true;
    }
    buffer.front = target;
    return mips ? generateMips(*target, result, failed) : true;
}

bool ProceduralGpuEvaluator::generateMips(Target& target, EvaluationResult& result, bool& failed) {
    if (!mipBox) {
        mipBox = std::make_unique<Program>();
        if (!createProgram(*mipBox, MIP_BOX_FRAGMENT, "mip chain", result)) {
            mipBox.reset();
            failed = true;
            return true;
        }
    }
    if (mipBox->shader->isPending()) return false;
    if (!mipBox->object && !loadObject(mipBox->object, *mipBox->shader)) {
        failed = true;
        fail(result, "mip chain: the device refused its pipeline", -1, mipBox->log);
        return true;
    }
    const std::pair<int, int> slot = mipBox->shader->shaderData.getTextureIndexByName("t_source");
    const int levels = mipLevels(target.size);
    const auto box = [&](TextureRender& into, int level, TextureRender& from, float fromLevel) {
        const float block[4] = {fromLevel, 0.0f, 0.0f, 0.0f};
        return drawInto(*mipBox, into, level, block, sizeof(block), {{slot, &from}}, true);
    };
    // each level is a box of the one above, reduced into the other texture and copied back
    for (int level = 1; level < levels; level++) {
        if (!box(*target.reduce, level, *target.texture, (float)(level - 1)) || !box(*target.texture, level, *target.reduce, (float)level)) {
            failed = true;
            fail(result, "mip chain: the device has no pipeline for it", -1, mipBox->log);
            return true;
        }
    }
    return true;
}

bool ProceduralGpuEvaluator::drawQuantize(size_t index, EvaluationResult& result, bool& failed) {
    if (!quantize) {
        quantize = std::make_unique<Program>();
        if (!createProgram(*quantize, QUANTIZE_FRAGMENT, "quantize", result)) {
            quantize.reset();
            failed = true;
            return true;
        }
    }
    if (quantize->shader->isPending()) return false;
    if (!quantize->object && !loadObject(quantize->object, *quantize->shader)) {
        failed = true;
        fail(result, "quantize: the device refused its pipeline", -1, quantize->log);
        return true;
    }
    if (!frameHasRoom(0)) return false;
    const PlannedOutput& output = plan.outputs[index];
    Target* source = buffers[(size_t)output.buffer]->front;
    if (!source) {
        failed = true;
        fail(result, "output " + output.name + " has no buffer", -1, {});
        return true;
    }
    // drawn into a texture of its own and swapped in, so the previous image stays whole until then (R7)
    std::unique_ptr<TextureRender>& shown = outputs[output.name];
    std::unique_ptr<TextureRender> next;
    TextureRender* into = shown.get();
    if (!shown || shown->getWidth() != plan.size) {
        next = makeTarget(plan.size, ColorFormat::RGBA, 1);
        if (!next) {
            failed = true;
            fail(result, "cannot make an image of " + std::to_string(plan.size), -1, {});
            return true;
        }
        into = next.get();
    }
    const std::pair<int, int> slot = quantize->shader->shaderData.getTextureIndexByName("t_source");
    if (!drawInto(*quantize, *into, 0, nullptr, 0, {{slot, source->texture.get()}}, false)) {
        if (quantize->object->isDrawWaiting()) return false;
        failed = true;
        fail(result, "quantize: the device has no pipeline for it", -1, quantize->log);
        return true;
    }
    if (next) {
        if (shown) shown->destroyTexture();
        shown = std::move(next);
    }
    return true;
}

bool ProceduralGpuEvaluator::advance(double budgetMs, EvaluationResult& result) {
    const auto start = std::chrono::steady_clock::now();
    const auto outOfBudget = [&]() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() >= budgetMs;
    };
    for (;;) {
        bool failed = false;
        switch (phase) {
            case Phase::Idle:
                return true;
            case Phase::Compile:
                if (nextPass >= plan.passes.size()) {
                    phase = Phase::Draw;
                    continue;
                }
                if (!compilePass(nextPass, result)) return true;
                nextPass++;
                break;
            case Phase::Draw:
                if (nextStep >= plan.steps.size()) {
                    phase = Phase::Quantize;
                    continue;
                }
                if (!drawStep(nextStep, result, failed)) return false;
                if (failed) return true;
                nextStep++;
                break;
            case Phase::Quantize:
                if (nextOutput >= plan.outputs.size()) {
                    for (auto it = outputs.begin(); it != outputs.end();) {
                        const bool planned = std::any_of(plan.outputs.begin(), plan.outputs.end(), [&](const PlannedOutput& output) { return output.name == it->first; });
                        if (planned) {
                            ++it;
                        } else {
                            it->second->destroyTexture();
                            it = outputs.erase(it);
                        }
                    }
                    if (!plan.readBack) {
                        result = EvaluationResult();
                        result.ok = true;
                        phase = Phase::Idle;
                        return true;
                    }
                    drawnFrame = SystemRender::getFrameCount();
                    phase = Phase::WaitCommit;
                    continue;
                }
                if (!drawQuantize(nextOutput, result, failed)) return false;
                if (failed) return true;
                nextOutput++;
                break;
            case Phase::WaitCommit:
                // a read back sees what a committed frame drew
                if (SystemRender::getFrameCount() == drawnFrame) return false;
                phase = Phase::ReadBack;
                continue;
            case Phase::ReadBack: {
                EvaluationResult done;
                done.ok = true;
                for (const PlannedOutput& output : plan.outputs) {
                    OutputImage image;
                    image.name = output.name;
                    image.width = plan.size;
                    image.height = plan.size;
                    std::string error = injectedReadBackFailure ? "injected by JASPER_TEST_PROCMAT_DEVICE_FAIL" : "";
                    if (injectedReadBackFailure || !FrameReadback::readTexture(*outputs[output.name], plan.size, plan.size, 4, image.rgba, &error)) {
                        fail(result, "cannot read " + output.name + " back: " + error, -1, {});
                        return true;
                    }
                    done.outputs.push_back(std::move(image));
                }
                result = std::move(done);
                phase = Phase::Idle;
                return true;
            }
        }
        if (outOfBudget()) return false;
    }
}

TextureRender* ProceduralGpuEvaluator::outputTexture(const std::string& name) const {
    auto it = outputs.find(name);
    return it == outputs.end() ? nullptr : it->second.get();
}

std::unique_ptr<TextureRender> ProceduralGpuEvaluator::takeOutputTexture(const std::string& name) {
    auto it = outputs.find(name);
    if (it == outputs.end()) return nullptr;
    std::unique_ptr<TextureRender> taken = std::move(it->second);
    outputs.erase(it);
    return taken;
}
