// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Stateful effects (phase 6, docs/html-ui/phase-6-stateful-effects.md): the state image pool, and
// EffectRenderer's side of a simulation: allocating its images, the clock rule (live, the substep
// cap, the pinned-clock replay), the per-step pointer and splats, and the passes.

#include "EffectSimulator.h"
#include "EffectRegistry.h"
#include "EffectRenderer.h"

#include "htmlui/RmlFilterContext.h"
#include "htmlui/RmlRenderer.h"

#include "Engine.h"
#include "Log.h"
#include "pool/ShaderPool.h"
#include "render/ShaderRender.h"
#include "render/SystemRender.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

using namespace jasperengine;
using namespace jasperengine::htmlui;

namespace {

    uint32_t& unsupportedMask(){
        static uint32_t mask = 0;
        return mask;
    }

    // every live image (a view loss destroys them, held or not)
    std::vector<std::weak_ptr<EffectStateImage>>& registry(){
        static std::vector<std::weak_ptr<EffectStateImage>> images;
        return images;
    }

    // u_fs_effectStep, member for member: 31 x vec4 = 496 bytes
    struct EffectStepBlock {
        float target[4], info[4], clock[4], pointer[4], pointerPrev[4], pointerVel[4], splatInfo[4];
        float splatPos[8][4], splatVel[8][4], splatColor[8][4];
    };
    static_assert(sizeof(EffectStepBlock) == 496, "u_fs_effectStep is 496 bytes");

    // u_vs_effectScatter
    struct EffectScatterBlock {
        float target[4];
        float place[4];
    };

    void set4(float out[4], float x, float y, float z, float w){
        out[0] = x;
        out[1] = y;
        out[2] = z;
        out[3] = w;
    }

    uint64_t fnv(uint64_t h, const void* data, size_t size){
        const unsigned char* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; i++){
            h ^= p[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    template<typename T>
    uint64_t fnvValue(uint64_t h, const T& value){
        return fnv(h, &value, sizeof(T));
    }

    Rml::Rectanglei fullRectOf(int width, int height){
        return Rml::Rectanglei::FromSize(Rml::Vector2i(std::max(width, 1), std::max(height, 1)));
    }

    // a context point in the area's local CSS px (through the element's transform)
    Rml::Vector2f toLocal(Rml::Element* element, Rml::Vector2f point, Rml::Vector2f areaOrigin){
        Rml::Vector2f p = point;
        element->Project(p);
        return p - areaOrigin;
    }

    // a box state's side: the area (CSS px) times the scale, the larger side at most `max` (the
    // aspect kept)
    int stateSide(const EffectStateDef& st, Rml::Vector2f area, bool width){
        if (st.fixedSize)
            return width ? st.width : st.height;
        float scale = st.boxScale;
        const float largest = std::max(area.x, area.y) * scale;
        if (largest > (float)st.boxMax)
            scale *= (float)st.boxMax / largest;
        const int side = (int)std::lround((width ? area.x : area.y) * scale);
        return std::clamp(side, 1, st.boxMax);
    }

}

ColorFormat jasperengine::htmlui::colorFormatOf(EffectStateFormat format){
    switch (format){
        case EffectStateFormat::RGBA8: return ColorFormat::RGBA;
        case EffectStateFormat::RGBA16F: return ColorFormat::RGBA16F;
        case EffectStateFormat::RGBA32F: return ColorFormat::RGBA32F;
        case EffectStateFormat::R32F: return ColorFormat::R32F;
        case EffectStateFormat::RG16F: return ColorFormat::RG16F;
        case EffectStateFormat::R16F: return ColorFormat::R16F;
    }
    return ColorFormat::RGBA16F;
}

// --- EffectStatePool ---

EffectStatePool& EffectStatePool::get(){
    // outlives RmlUi; its images are destroyed by destroyAll (shutdown, view loss)
    static EffectStatePool* pool = new EffectStatePool();
    return *pool;
}

void EffectStatePool::setUnsupportedFormatsForTesting(uint32_t mask){
    unsupportedMask() = mask;
}

bool EffectStatePool::supports(EffectStateFormat format, bool blend){
    if (unsupportedMask() & (1u << (unsigned)format))
        return false;
    const RenderFeatures& f = SystemRender::getFeatures();
    switch (format){
        case EffectStateFormat::RGBA8: return true;
        case EffectStateFormat::RGBA16F: return f.renderRGBA16F && (!blend || f.blendRGBA16F);
        case EffectStateFormat::RGBA32F: return f.renderRGBA32F && (!blend || f.blendRGBA32F);
        case EffectStateFormat::R32F: return f.renderR32F && (!blend || f.blendR32F);
        case EffectStateFormat::RG16F: return f.renderRG16F && (!blend || f.blendRG16F);
        case EffectStateFormat::R16F: return f.renderR16F && (!blend || f.blendR16F);
    }
    return false;
}

std::shared_ptr<EffectStateImage> EffectStatePool::acquire(EffectStateFormat format, int width, int height, bool linear, bool repeat){
    for (size_t i = pooled.size(); i-- > 0;){
        const std::shared_ptr<EffectStateImage>& candidate = pooled[i];
        if (candidate->format == format && candidate->width == width && candidate->height == height && candidate->linear == linear &&
            candidate->repeat == repeat){
            std::shared_ptr<EffectStateImage> image = candidate;
            pooled.erase(pooled.begin() + (long)i);
            live++;
            liveBytesCount += image->bytes;
            return image;
        }
    }
    auto image = std::make_shared<EffectStateImage>();
    image->format = format;
    image->width = width;
    image->height = height;
    image->linear = linear;
    image->repeat = repeat;
    image->bytes = (size_t)width * (size_t)height * (size_t)effectStateFormatBytes(format);
    FramebufferDesc desc;
    desc.width = width;
    desc.height = height;
    desc.color = colorFormatOf(format);
    desc.depth = DepthStencilFormat::NONE;
    desc.sampleCount = 1;
    desc.minFilter = desc.magFilter = linear ? TextureFilter::LINEAR : TextureFilter::NEAREST;
    desc.wrapU = desc.wrapV = repeat ? TextureWrap::REPEAT : TextureWrap::CLAMP_TO_EDGE;
    desc.label = "html ui effect state";
    if (!image->framebuffer.create(desc))
        return nullptr;
    created++;
    live++;
    liveBytesCount += image->bytes;
    auto& all = registry();
    all.erase(std::remove_if(all.begin(), all.end(), [](const std::weak_ptr<EffectStateImage>& w){ return w.expired(); }), all.end());
    all.push_back(image);
    return image;
}

void EffectStatePool::release(std::shared_ptr<EffectStateImage> image){
    if (!image)
        return;
    if (live > 0)
        live--;
    liveBytesCount = liveBytesCount >= image->bytes ? liveBytesCount - image->bytes : 0;
    // an image of an earlier generation (a view loss) is gone: dropped, not pooled
    if (!image->framebuffer.isCreated()){
        image->framebuffer.destroy();
        return;
    }
    image->releasedFrame = frame;
    image->cleared = false;
    pooled.push_back(std::move(image));
}

size_t EffectStatePool::pooledBytes() const{
    size_t bytes = 0;
    for (const auto& image : pooled)
        bytes += image->bytes;
    return bytes;
}

void EffectStatePool::trim(uint64_t idleFrames){
    for (size_t i = pooled.size(); i-- > 0;){
        if (idleFrames == 0 || frame - pooled[i]->releasedFrame >= idleFrames){
            pooled[i]->framebuffer.destroy();
            pooled.erase(pooled.begin() + (long)i);
        }
    }
}

void EffectStatePool::beginFrame(){
    frame++;
    trim(kIdleFrames);
    // beyond the cap: the least recently released first
    size_t bytes = pooledBytes();
    while (bytes > kMaxPooledBytes && !pooled.empty()){
        auto oldest = std::min_element(pooled.begin(), pooled.end(), [](const auto& a, const auto& b){ return a->releasedFrame < b->releasedFrame; });
        bytes -= (*oldest)->bytes;
        (*oldest)->framebuffer.destroy();
        pooled.erase(oldest);
    }
}

void EffectStatePool::destroyAll(){
    for (auto& image : pooled)
        image->framebuffer.destroy();
    pooled.clear();
    for (auto& weak : registry()){
        if (auto image = weak.lock())
            image->framebuffer.destroy();
    }
    registry().clear();
    generationCounter++;
}

// --- EffectRenderer: the simulations ---

struct EffectRenderer::StepInput {
    double time = 0.0;          // the simulation time at the end of the step
    float dt = 0.f;
    int64_t stepNumber = 0;     // steps since the reset, this one included
    bool warm = false;
    int stepInFrame = 0, stepsInFrame = 1;
    Rml::Vector2f pointer{0.f, 0.f}, pointerPrev{0.f, 0.f}, velocity{0.f, 0.f};
    bool known = false, inside = false, down = false, pressed = false, released = false;
    std::vector<EffectSplatData> splats;
};

TextureRender* EffectRenderer::blackPlaceholder(){
    if (!blackCreated){
        static unsigned char texel[4] = {0, 0, 0, 0};
        void* data[1] = {texel};
        size_t size[1] = {4};
        blackCreated = black.createTexture("html ui effect state placeholder", 1, 1, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, data, size,
                                           TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE);
    }
    return blackCreated ? &black : &renderer.placeholderWhite;
}

EffectRenderer::Program* EffectRenderer::formProgram(EffectInstance& instance, uint32_t properties){
    const ShaderKey key = EffectRegistry::get().shaderKey(*instance.manifest, instance.filterForm);
    const uint16_t customId = ShaderPool::getCustomIdFromKey(key);
    std::shared_ptr<ShaderRender> shader = ShaderPool::get(ShaderType::UI_EFFECT, ShaderPool::getPropertiesFromKey(key) | properties, customId);
    if (!shader || !shader->isCreated()){
        EffectRuntime::get().mutableStats().compileCalls++;
        EffectRuntime::get().mutableFrameStats().compileCalls++;
        return nullptr;
    }
    return loadProgram(shader, true, instance.manifest);
}

void EffectRenderer::bindStates(EffectInstance& instance, Program& program, int writingState){
    EffectSimulation* simulation = instance.state ? instance.state->simulation.get() : nullptr;
    for (size_t i = 0; i < program.stateSlots.size(); i++){
        const auto& slot = program.stateSlots[i];
        if (slot.first < 0)
            continue;
        TextureRender* texture = nullptr;
        // a ping-pong target is read from its current image while the other is written; a
        // single-image target (writingState) is not read while it is written
        if (simulation && simulation->allocated && i < simulation->states.size() && (int)i != writingState){
            const EffectSimulation::Images& images = simulation->states[i];
            if (images.image[images.current])
                texture = &images.image[images.current]->framebuffer.getRender().getColorTexture();
        }
        program.object.addTexture(slot, ShaderStageType::FRAGMENT, texture ? texture : blackPlaceholder());
    }
}

bool EffectRenderer::clippedOut(const EffectInstance& instance) const{
    // the area's corners in document px through the draw's transform (projection x element),
    // against the scissor and the context
    const Rml::Matrix4f& m = renderer.getCurrentTransform();
    const Rml::Vector2f o = instance.areaOrigin;
    const Rml::Vector2f size = instance.areaSize;
    const Rml::Vector2f corners[4] = {o, o + Rml::Vector2f(size.x, 0.f), o + Rml::Vector2f(0.f, size.y), o + size};
    const float w = (float)std::max(renderer.contextSize.x, 1), h = (float)std::max(renderer.contextSize.y, 1);
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    for (const Rml::Vector2f& c : corners){
        const Rml::Vector4f clip = m * Rml::Vector4f(c.x, c.y, 0.f, 1.f);
        if (clip.w <= 1e-6f)
            return false;   // behind the eye of a perspective: not decided here
        const float nx = clip.x / clip.w, ny = clip.y / clip.w;
        const float x = (nx + 1.f) * 0.5f * w;
        const float y = (Engine::isOpenGL() ? (1.f + ny) : (1.f - ny)) * 0.5f * h;
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
    }
    float left = 0.f, top = 0.f, right = w, bottom = h;
    if (renderer.scissorEnabled){
        left = std::max(left, (float)renderer.scissorRegion.Left());
        top = std::max(top, (float)renderer.scissorRegion.Top());
        right = std::min(right, (float)renderer.scissorRegion.Right());
        bottom = std::min(bottom, (float)renderer.scissorRegion.Bottom());
    }
    return maxX <= left || minX >= right || maxY <= top || minY >= bottom;
}

void EffectRenderer::clearImage(EffectStateImage& image){
    if (renderer.bindTarget(image.framebuffer, LoadAction::CLEAR))
        image.cleared = true;
}

bool EffectRenderer::allocateStates(EffectInstance& instance, EffectSimulation& simulation, bool resample){
    const EffectManifest& m = *instance.manifest;
    EffectStatePool& pool = EffectStatePool::get();
    std::vector<EffectSimulation::Images> next(m.states.size());
    size_t bytes = 0;
    for (size_t i = 0; i < m.states.size(); i++){
        const EffectStateDef& st = m.states[i];
        EffectSimulation::Images& images = next[i];
        images.width = stateSide(st, instance.areaSize, true);
        images.height = stateSide(st, instance.areaSize, false);
        const bool keep = resample && i < simulation.states.size() && simulation.states[i].width == images.width &&
                          simulation.states[i].height == images.height;
        if (keep){
            images = simulation.states[i];
            simulation.states[i] = EffectSimulation::Images();
        }else{
            for (int k = 0; k < (st.single ? 1 : 2); k++){
                images.image[k] = pool.acquire(st.format, images.width, images.height, st.linear, st.repeat);
                if (!images.image[k]){
                    for (auto& allocated : next)
                        for (auto& image : allocated.image)
                            if (image)
                                pool.release(std::move(image));
                    return false;
                }
                clearImage(*images.image[k]);
            }
            // a resize that keeps the look: the old image redrawn into the new size
            if (resample && st.resample && !st.fixedSize && i < simulation.states.size() && simulation.states[i].image[0]){
                const EffectSimulation::Images& old = simulation.states[i];
                PipelineState state;
                state.target.color = colorFormatOf(st.format);
                state.target.depth = DepthStencilFormat::NONE;
                state.target.sampleCount = 1;
                state.blend = BlendMode::NONE;
                RmlRenderer::PostDraw draw;
                draw.variant = (int)HtmlUIPostVariant::PASSTHROUGH;
                draw.source = &old.image[old.current]->framebuffer.getRender().getColorTexture();
                renderer.drawPost(images.image[0]->framebuffer, LoadAction::DONTCARE, draw, state);
                images.current = 0;
            }
        }
        for (const auto& image : images.image)
            if (image)
                bytes += image->bytes;
    }
    // what is not kept goes back
    for (auto& images : simulation.states)
        for (auto& image : images.image)
            if (image)
                pool.release(std::move(image));
    simulation.states = std::move(next);
    simulation.bytes = bytes;
    simulation.allocated = true;
    simulation.poolGeneration = pool.generation();
    simulation.areaSize = instance.areaSize;
    return true;
}

bool EffectRenderer::runPass(EffectInstance& instance, EffectSimulation& simulation, Program& drawProgram, RmlFilterContext* filter,
                             const EffectPassDef& pass, int passIndex, int iteration, const StepInput& input){
    const EffectManifest& m = *instance.manifest;
    const bool scatter = pass.scatterState >= 0;
    Program* program = formProgram(instance, scatter ? 4u : 2u);
    if (!program)
        return false;
    EffectSimulation::Images& target = simulation.states[(size_t)pass.state];
    const EffectStateDef& def = m.states[(size_t)pass.state];
    const int writeIndex = def.single ? 0 : 1 - target.current;
    EffectStateImage& image = *target.image[writeIndex];

    // additive targets start cleared; the others are overwritten whole
    if (!renderer.bindTarget(image.framebuffer, pass.additive ? LoadAction::CLEAR : LoadAction::DONTCARE))
        return false;
    const Rml::Rectanglei full = fullRectOf(target.width, target.height);
    renderer.applyPassViewport(full);
    renderer.applyPassScissor(full);

    bindStates(instance, *program, def.single ? pass.state : -1);
    if (filter && program->sourceSlot.first >= 0)
        program->object.addTexture(program->sourceSlot, ShaderStageType::FRAGMENT, &filter->primary().getRender().getColorTexture());
    if (program->blurredSlot.first >= 0)
        program->object.addTexture(program->blurredSlot, ShaderStageType::FRAGMENT,
                                   filter ? &filter->primary().getRender().getColorTexture() : &renderer.placeholderWhite);
    unsigned int vertices = 3;
    EffectScatterBlock scatterBlock{};
    if (scatter){
        const EffectSimulation::Images& place = simulation.states[(size_t)pass.scatterState];
        if (program->scatterPlaceSlot.first >= 0)
            program->object.addTexture(program->scatterPlaceSlot, ShaderStageType::VERTEX,
                                       &place.image[place.current]->framebuffer.getRender().getColorTexture());
        set4(scatterBlock.target, (float)target.width, (float)target.height, 1.f / (float)target.width, 1.f / (float)target.height);
        set4(scatterBlock.place, (float)place.width, (float)place.height, 0.f, 0.f);
        vertices = (unsigned int)(6 * place.width * place.height);
    }

    PipelineState state;
    state.target.color = colorFormatOf(def.format);
    state.target.depth = DepthStencilFormat::NONE;
    state.target.sampleCount = 1;
    state.blend = pass.additive ? BlendMode::ADDITIVE_PREMULTIPLIED : BlendMode::NONE;
    const uint64_t misses = SystemRender::getPipelineCacheMisses();
    if (!program->object.beginDraw(state))
        return false;
    EffectRuntime& runtime = EffectRuntime::get();
    const uint64_t createdPipelines = SystemRender::getPipelineCacheMisses() - misses;
    runtime.mutableStats().pipelinesCreated += createdPipelines;
    runtime.mutableFrameStats().pipelinesCreated += createdPipelines;

    EffectStepBlock step{};
    set4(step.target, (float)target.width, (float)target.height, 1.f / (float)target.width, 1.f / (float)target.height);
    set4(step.info, (float)passIndex, (float)iteration, (float)input.stepNumber, input.warm ? 1.f : 0.f);
    set4(step.clock, (float)input.time, input.dt, (float)input.stepInFrame, (float)input.stepsInFrame);
    set4(step.pointer, input.pointer.x, input.pointer.y, input.inside ? 1.f : 0.f, input.down ? 1.f : 0.f);
    set4(step.pointerPrev, input.pointerPrev.x, input.pointerPrev.y, input.known ? 1.f : 0.f, input.pressed ? 1.f : 0.f);
    set4(step.pointerVel, input.velocity.x, input.velocity.y, input.released ? 1.f : 0.f, 0.f);
    const size_t splats = std::min<size_t>(input.splats.size(), 8);
    set4(step.splatInfo, (float)splats, 0.f, 0.f, 0.f);
    for (size_t i = 0; i < splats; i++){
        const EffectSplatData& sp = input.splats[i];
        set4(step.splatPos[i], sp.position.x, sp.position.y, sp.radius, sp.strength);
        set4(step.splatVel[i], sp.velocity.x, sp.velocity.y, 0.f, 0.f);
        set4(step.splatColor[i], sp.color[0], sp.color[1], sp.color[2], sp.color[3]);
    }
    // the uniform data applied (sokol aligns each block to 256 bytes in the frame's buffer)
    size_t uniformBytes = 0;
    auto applied = [&uniformBytes](size_t size){ uniformBytes += (size + 255u) & ~(size_t)255u; };
    // the frame block and the parameters: the draw's (the same block declarations)
    if (program->frameSlot >= 0){
        program->object.applyUniformBlock(program->frameSlot, sizeof(EffectFrameBlock), &instance.frame);
        applied(sizeof(EffectFrameBlock));
    }
    if (program->params.slot >= 0 && !instance.params.empty()){
        if (program->params.data.size() == instance.params.size() || program->params.data.size() == 0)
            program->object.applyUniformBlock(program->params.slot, (unsigned int)instance.params.size(), instance.params.data());
        else{
            std::vector<uint8_t> bytes(program->params.data.size(), 0);
            std::memcpy(bytes.data(), instance.params.data(), std::min(bytes.size(), instance.params.size()));
            program->object.applyUniformBlock(program->params.slot, (unsigned int)bytes.size(), bytes.data());
        }
        applied(std::max(program->params.data.size(), instance.params.size()));
    }
    if (program->trailSlot >= 0){
        program->object.applyUniformBlock(program->trailSlot, sizeof(instance.trail), instance.trail);
        applied(sizeof(instance.trail));
    }
    if (program->ripplesSlot >= 0){
        program->object.applyUniformBlock(program->ripplesSlot, sizeof(instance.ripples), instance.ripples);
        applied(sizeof(instance.ripples));
    }
    if (program->stepSlot >= 0){
        program->object.applyUniformBlock(program->stepSlot, sizeof(EffectStepBlock), &step);
        applied(sizeof(EffectStepBlock));
    }
    if (scatter && program->scatterSlot >= 0){
        program->object.applyUniformBlock(program->scatterSlot, sizeof(EffectScatterBlock), &scatterBlock);
        applied(sizeof(EffectScatterBlock));
    }
    runtime.addSimulationUniformBytes(uniformBytes);
    program->object.draw(0, vertices, 1);
    renderer.stats.drawCalls++;

    if (!def.single)
        target.current = writeIndex;
    (scatter ? runtime.mutableStats().scatterPasses : runtime.mutableStats().stepPasses)++;
    (scatter ? runtime.mutableFrameStats().scatterPasses : runtime.mutableFrameStats().stepPasses)++;
    (void)drawProgram;
    return true;
}

void EffectRenderer::runStep(EffectInstance& instance, EffectSimulation& simulation, Program& drawProgram, RmlFilterContext* filter,
                             const StepInput& input, bool initPasses){
    const EffectManifest& m = *instance.manifest;
    for (size_t p = 0; p < m.passes.size(); p++){
        const EffectPassDef& pass = m.passes[p];
        if (pass.init != initPasses)
            continue;
        int repeat = pass.repeat;
        if (!pass.repeatParam.empty() && instance.snapshot){
            const int index = m.paramIndex(pass.repeatParam);
            if (index >= 0 && index < (int)instance.snapshot->values.size())
                repeat = (int)std::lround(instance.snapshot->values[(size_t)index].f);
        }
        repeat = std::clamp(repeat, 1, 64);
        for (int it = 0; it < repeat; it++){
            if (!runPass(instance, simulation, drawProgram, filter, pass, (int)p, it, input))
                return;
        }
    }
    if (!initPasses){
        simulation.stepsRun++;
        EffectRuntime::get().mutableStats().stepsRun++;
        EffectRuntime::get().mutableFrameStats().stepsRun++;
    }
}

void EffectRenderer::runInit(EffectInstance& instance, EffectSimulation& simulation, Program& drawProgram, RmlFilterContext* filter,
                             double resetTime){
    const EffectManifest& m = *instance.manifest;
    simulation.steps = 0;
    simulation.resetTime = resetTime;
    simulation.simTime = resetTime;
    StepInput init;
    init.time = resetTime - (double)m.warmupSteps * m.warmupDt;
    init.dt = 0.f;
    init.stepNumber = 0;
    runStep(instance, simulation, drawProgram, filter, init, true);
}

void EffectRenderer::runWarmStep(EffectInstance& instance, EffectSimulation& simulation, Program& drawProgram, RmlFilterContext* filter,
                                 double resetTime, int w){
    const EffectManifest& m = *instance.manifest;
    StepInput warm;
    warm.warm = true;
    warm.dt = m.warmupDt;
    warm.time = resetTime - (double)(m.warmupSteps - 1 - w) * m.warmupDt;
    warm.stepNumber = 0;
    warm.stepInFrame = w;
    warm.stepsInFrame = m.warmupSteps;
    runStep(instance, simulation, drawProgram, filter, warm, false);
}

void EffectRenderer::runInitAndWarmup(EffectInstance& instance, EffectSimulation& simulation, Program& drawProgram, RmlFilterContext* filter,
                                      double resetTime){
    runInit(instance, simulation, drawProgram, filter, resetTime);
    for (int w = 0; w < instance.manifest->warmupSteps; w++)
        runWarmStep(instance, simulation, drawProgram, filter, resetTime, w);
}

bool EffectRenderer::advanceSimulation(EffectInstance& instance, Program& drawProgram, RmlFilterContext* filter){
    EffectRuntime& runtime = EffectRuntime::get();
    EffectSimulation* simulationPtr = runtime.simulationOf(instance);
    if (!simulationPtr || !instance.element)
        return true;
    EffectSimulation& simulation = *simulationPtr;
    const EffectManifest& m = *instance.manifest;
    // an empty area: nothing to simulate or draw
    if (instance.areaSize.x < 0.5f || instance.areaSize.y < 0.5f)
        return false;
    const uint64_t frame = runtime.getFrame();
    // drawn again this frame (a context rendered twice): stepped once
    if (simulation.lastAdvanceFrame == frame)
        return simulation.allocated && !simulation.replaying;
    // the device's formats (3.3)
    for (const EffectStateDef& st : m.states){
        bool blend = false;
        for (const EffectPassDef& pass : m.passes)
            blend = blend || (pass.state == m.stateIndex(st.name) && pass.additive);
        if (!EffectStatePool::supports(st.format, blend)){
            if (!simulation.refused){
                simulation.refused = true;
                runtime.reportStateful(simulation.document, "HTMLUI-FX-0017",
                                       "effect '" + m.name + "' keeps state '" + st.name + "' in " + effectStateFormatName(st.format) +
                                       " images this device cannot " + (blend ? "blend into" : "render to") + "; it is not drawn", true);
            }
            return false;
        }
    }
    if (!filter && clippedOut(instance)){
        simulation.paused = true;
        return false;
    }
    if (!runtime.admitSimulation(simulation))
        return false;
    // the passes' variants: until they are built (the editor compiles them in the background)
    // the effect waits, drawing nothing
    if (!formProgram(instance, 2u) || (m.hasScatter() && !formProgram(instance, 4u)))
        return false;

    const bool pinnedClock = EffectClock::isFixed();
    const double clock = EffectClock::now();
    const double T = clock * instance.timeScale + instance.timeOffset;
    const double h = 1.0 / std::max(1.f, m.rateHz);
    EffectStatePool& pool = EffectStatePool::get();

    // a view loss destroyed the images: restart
    if (simulation.allocated && simulation.poolGeneration != pool.generation()){
        for (auto& images : simulation.states)
            for (auto& image : images.image)
                image.reset();
        simulation.states.clear();
        simulation.allocated = false;
    }
    // a change of a @reseed parameter restarts the simulation
    std::string reseedKey;
    if (instance.snapshot){
        for (const std::string& param : m.reseedParams){
            const int index = m.paramIndex(param);
            if (index >= 0 && (size_t)index < instance.snapshot->canonical.size())
                reseedKey += instance.snapshot->canonical[(size_t)index].second + ";";
        }
    }
    bool reset = false;
    if (simulation.allocated && reseedKey != simulation.reseedKey){
        for (auto& images : simulation.states){
            for (auto& image : images.image)
                if (image)
                    clearImage(*image);
            images.current = 0;
        }
        reset = true;
    }
    simulation.reseedKey = reseedKey;
    if (!simulation.allocated){
        if (!allocateStates(instance, simulation, false))
            return false;
        reset = true;
    }else{
        // a resize of the box states: resample or restart (live), replay (pinned)
        bool resized = false, restart = false;
        for (size_t i = 0; i < m.states.size(); i++){
            const EffectStateDef& st = m.states[i];
            if (st.fixedSize)
                continue;
            if (stateSide(st, instance.areaSize, true) != simulation.states[i].width ||
                stateSide(st, instance.areaSize, false) != simulation.states[i].height){
                resized = true;
                restart = restart || !st.resample;
            }
        }
        if (resized){
            if (!allocateStates(instance, simulation, !pinnedClock && !restart))
                return false;
            reset = pinnedClock || restart;
        }
    }

    // the pointer this frame (area-local CSS px)
    Rml::Vector2f pointerContext(0.f, 0.f);
    bool pointerValid = false, pointerDown = false;
    uint64_t pressCount = 0;
    Rml::Context* context = instance.context ? instance.context : instance.element->GetContext();
    pointerValid = runtime.pointerOf(context, pointerContext, pointerDown, pressCount);
    const Rml::Vector2f pointerLocal = pointerValid ? toLocal(instance.element, pointerContext, instance.areaOrigin) : Rml::Vector2f(0.f, 0.f);
    auto insideArea = [&](Rml::Vector2f p){
        return p.x >= 0.f && p.y >= 0.f && p.x <= instance.areaSize.x && p.y <= instance.areaSize.y;
    };

    if (pinnedClock){
        // the state at T is defined (3.6): reset, @init, @warmup, then the steps of the replay window,
        // whenever what defines it changed
        std::vector<Rml::Vector4f> history;
        runtime.pointerHistory(instance, history);
        uint64_t key = 14695981039346656037ull;
        key = fnvValue(key, T);
        key = fnvValue(key, simulation.signature);
        for (const auto& images : simulation.states){
            key = fnvValue(key, images.width);
            key = fnvValue(key, images.height);
        }
        key = fnvValue(key, simulation.inputGeneration);
        if (instance.snapshot)
            for (const auto& entry : instance.snapshot->canonical)
                key = fnv(fnv(key, entry.first.data(), entry.first.size()), entry.second.data(), entry.second.size());
        for (const Rml::Vector4f& sample : history){
            const Rml::Vector2f local = toLocal(instance.element, Rml::Vector2f(sample.x, sample.y), instance.areaOrigin);
            key = fnvValue(key, local.x);
            key = fnvValue(key, local.y);
            key = fnvValue(key, sample.z);
        }
        key = fnvValue(key, pointerValid);
        key = fnvValue(key, pointerLocal.x);
        key = fnvValue(key, pointerLocal.y);
        key = fnvValue(key, pointerDown);
        key = fnvValue(key, instance.areaSize.x);
        key = fnvValue(key, instance.areaSize.y);
        // the images were reset under a running replay (a resize, a reseed, a view loss), or a live
        // warm-up was running: it restarts
        if (simulation.replaying && (reset || simulation.replayWarmOnly)){
            if (simulation.replayWarmOnly)
                reset = true;
            simulation.replaying = false;
            runtime.replayEnded();
        }
        if (!simulation.replaying && (!simulation.pinned || simulation.pinnedKey != key || reset)){
            for (auto& images : simulation.states){
                for (auto& image : images.image)
                    if (image)
                        clearImage(*image);
                images.current = 0;
            }
            const double window = std::max(0.0, std::min(T, (double)m.replaySeconds));
            simulation.replaying = true;
            simulation.replayWarmOnly = false;
            simulation.pinned = false;
            simulation.replayKey = key;
            simulation.replayUnit = 0;
            simulation.replayCount = (int64_t)std::floor(window / h + 1e-6);
            simulation.replayT = T;
            simulation.replayStart = T - (double)simulation.replayCount * h;
            simulation.replayHistory = history;
            simulation.replayLog = simulation.log;
            simulation.replayPointer = pointerLocal;
            simulation.replayPointerValid = pointerValid;
            simulation.replayPointerDown = pointerDown;
            runtime.replayStarted();
            simulation.replays++;
            runtime.mutableStats().replays++;
            runtime.mutableFrameStats().replays++;
        }
        if (simulation.replaying){
            const double start = simulation.replayStart;
            const double TR = simulation.replayT;
            const std::vector<Rml::Vector4f>& hist = simulation.replayHistory;
            // the pointer along the replay: the history at age T - t (newest first)
            auto pointerAt = [&](double t, Rml::Vector2f& p) -> bool {
                if (hist.empty()){
                    p = simulation.replayPointer;
                    return simulation.replayPointerValid;
                }
                const float age = (float)std::max(0.0, TR - t);
                if (age <= hist.front().z){
                    p = toLocal(instance.element, Rml::Vector2f(hist.front().x, hist.front().y), instance.areaOrigin);
                    return true;
                }
                for (size_t i = 0; i + 1 < hist.size(); i++){
                    const Rml::Vector4f& n = hist[i];
                    const Rml::Vector4f& o = hist[i + 1];
                    if (age >= n.z && age <= o.z){
                        const float k = o.z > n.z ? (age - n.z) / (o.z - n.z) : 0.f;
                        const Rml::Vector2f c(n.x + (o.x - n.x) * k, n.y + (o.y - n.y) * k);
                        p = toLocal(instance.element, c, instance.areaOrigin);
                        return true;
                    }
                }
                p = toLocal(instance.element, Rml::Vector2f(hist.back().x, hist.back().y), instance.areaOrigin);
                return true;
            };
            const int64_t warm = m.warmupSteps;
            const int64_t count = simulation.replayCount;
            // the units of the replay, until the frame's uniform budget is spent (one at least)
            while (simulation.replayUnit <= warm + count){
                const int64_t unit = simulation.replayUnit;
                if (unit == 0){
                    runInit(instance, simulation, drawProgram, filter, start);
                    simulation.replayPreviousKnown = pointerAt(start, simulation.replayPrevious);
                }else if (unit <= warm){
                    runWarmStep(instance, simulation, drawProgram, filter, start, (int)(unit - 1));
                }else{
                    const int64_t k = unit - warm - 1;
                    StepInput input;
                    input.time = start + (double)(k + 1) * h;
                    input.dt = (float)h;
                    input.stepNumber = k + 1;
                    input.stepInFrame = (int)k;
                    input.stepsInFrame = (int)count;
                    input.known = pointerAt(input.time, input.pointer);
                    input.pointerPrev = simulation.replayPreviousKnown ? simulation.replayPrevious : input.pointer;
                    input.velocity = (input.known && simulation.replayPreviousKnown) ? (input.pointer - input.pointerPrev) * (float)(1.0 / h)
                                                                                      : Rml::Vector2f(0.f, 0.f);
                    input.inside = input.known && insideArea(input.pointer);
                    input.down = simulation.replayPointerDown;
                    // the splats logged inside this step's interval (one at T lands on the last step)
                    const double from = input.time - h;
                    for (const auto& logged : simulation.replayLog){
                        if ((logged.first > from + 1e-9 && logged.first <= input.time + 1e-9) || (k + 1 == count && logged.first > input.time))
                            input.splats.push_back(logged.second);
                    }
                    runStep(instance, simulation, drawProgram, filter, input, false);
                    simulation.steps = k + 1;
                    simulation.replayPrevious = input.pointer;
                    simulation.replayPreviousKnown = input.known;
                }
                simulation.replayUnit++;
                if (simulation.replayUnit <= warm + count && runtime.simulationUniformBytes() >= EffectRuntime::kSimulationUniformBudget)
                    break;
            }
            if (simulation.replayUnit <= warm + count){
                // not done: continued next frame, not drawn meanwhile
                simulation.lastAdvanceFrame = frame;
                simulation.lastTime = T;
                simulation.paused = false;
                runtime.requestRedraw(context);
                return false;
            }
            simulation.replaying = false;
            runtime.replayEnded();
            simulation.steps = count;
            simulation.simTime = TR;
            simulation.pinned = true;
            simulation.pinnedKey = simulation.replayKey;
            if (simulation.pinnedKey != key)
                runtime.requestRedraw(context);   // the inputs moved on meanwhile: another replay next frame
        }
        simulation.lastAdvanceFrame = frame;
        simulation.lastTime = T;
        simulation.paused = false;
        simulation.anchored = false;
        simulation.pointer = pointerLocal;
        simulation.pointerKnown = pointerValid;
        simulation.pointerDown = pointerDown;
        simulation.pressSeen = pressCount;
        return true;
    }

    // the live clock
    if (simulation.pinned){
        // the pinned clock was released: the simulation continues from the pinned state
        simulation.pinned = false;
        simulation.log.clear();
    }
    if (simulation.replaying && !simulation.replayWarmOnly){
        // a pinned replay was running: its images are partial, the simulation restarts
        simulation.replaying = false;
        runtime.replayEnded();
        for (auto& images : simulation.states){
            for (auto& image : images.image)
                if (image)
                    clearImage(*image);
            images.current = 0;
        }
        reset = true;
    }
    if (reset){
        if (simulation.replaying){
            simulation.replaying = false;
            runtime.replayEnded();
        }
        // @init, then the @warmup steps within the frame's uniform budget (the rest next frames,
        // not drawn meanwhile)
        simulation.replaying = true;
        simulation.replayWarmOnly = true;
        simulation.replayUnit = 0;
        simulation.replayCount = 0;
        simulation.replayStart = T;
        runtime.replayStarted();
    }
    if (simulation.replaying){
        const int64_t warm = m.warmupSteps;
        while (simulation.replayUnit <= warm){
            if (simulation.replayUnit == 0)
                runInit(instance, simulation, drawProgram, filter, simulation.replayStart);
            else
                runWarmStep(instance, simulation, drawProgram, filter, simulation.replayStart, (int)(simulation.replayUnit - 1));
            simulation.replayUnit++;
            if (simulation.replayUnit <= warm && runtime.simulationUniformBytes() >= EffectRuntime::kSimulationUniformBudget)
                break;
        }
        if (simulation.replayUnit <= warm){
            simulation.lastAdvanceFrame = frame;
            simulation.lastTime = T;
            simulation.paused = false;
            runtime.requestRedraw(context);
            return false;
        }
        simulation.replaying = false;
        simulation.replayWarmOnly = false;
        runtime.replayEnded();
        simulation.anchored = false;
    }
    const bool resumed = simulation.lastAdvanceFrame == UINT64_MAX || simulation.lastAdvanceFrame + 1 != frame;
    if (!simulation.anchored || resumed || T < simulation.lastTime || instance.timeScale <= 0.f){
        simulation.epoch = T - (double)simulation.steps * h;
        simulation.anchored = true;
    }
    int64_t want = (int64_t)std::floor((T - simulation.epoch) / h + 1e-9);
    int64_t n = want - simulation.steps;
    if (n > m.maxSubsteps){
        // the substep cap: the excess is dropped (the simulation slows down instead of spiralling)
        n = m.maxSubsteps;
        simulation.epoch = T - (double)(simulation.steps + n) * h;
    }
    if (n < 0){
        n = 0;
        simulation.epoch = T - (double)simulation.steps * h;
    }
    const bool pressed = pointerValid && pressCount != simulation.pressSeen;
    const bool released = simulation.pointerDown && !pointerDown;
    for (int64_t k = 0; k < n; k++){
        StepInput input;
        input.time = simulation.epoch + (double)(simulation.steps + 1) * h;
        input.dt = (float)h;
        input.stepNumber = simulation.steps + 1;
        input.stepInFrame = (int)k;
        input.stepsInFrame = (int)n;
        // the frame's movement spread over its steps
        const float f = (float)(k + 1) / (float)n;
        input.known = pointerValid;
        if (pointerValid){
            const Rml::Vector2f from = simulation.pointerKnown ? simulation.pointer : pointerLocal;
            const Rml::Vector2f prevStep = simulation.pointerKnown ? from + (pointerLocal - from) * ((float)k / (float)n) : pointerLocal;
            input.pointer = from + (pointerLocal - from) * f;
            input.pointerPrev = prevStep;
            input.velocity = (input.pointer - input.pointerPrev) * (float)(1.0 / h);
        }
        input.inside = pointerValid && insideArea(input.pointer);
        input.down = pointerDown;
        input.pressed = k == 0 && pressed;
        input.released = k == 0 && released;
        if (k == 0){
            input.splats = simulation.pending;
            simulation.pending.clear();
        }
        runStep(instance, simulation, drawProgram, filter, input, false);
        simulation.steps++;
        simulation.simTime = input.time;
    }
    if (n > 0 || !pointerValid){
        simulation.pointer = pointerLocal;
        simulation.pointerKnown = pointerValid;
        simulation.pointerDown = pointerDown;
        simulation.pressSeen = pressCount;
    }
    simulation.lastAdvanceFrame = frame;
    simulation.lastTime = T;
    simulation.paused = false;
    // a running clock redraws the effect every frame
    runtime.requestRedraw(context);
    return true;
}
