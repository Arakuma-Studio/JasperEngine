// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The effects on the RmlRenderer (phase-5 spec 3.7): paint draws and filter passes of the
// UI_EFFECT variants, with the frame, params, trail and ripple blocks.

#include "EffectRenderer.h"
#include "EffectRegistry.h"

#include "htmlui/RmlFilterContext.h"
#include "htmlui/RmlRenderer.h"

#include "Log.h"
#include "pool/ShaderPool.h"
#include "render/ShaderRender.h"
#include "render/SystemRender.h"
#include "texture/Framebuffer.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::htmlui;

namespace {

    // u_vs_effectVertex
    struct VertexBlock {
        float transform[16];
        float translate[4];
    };
    static_assert(sizeof(VertexBlock) == 80, "u_vs_effectVertex is 80 bytes");

    // u_fs_effectParams of the bright pass (_bright.frag)
    struct BrightBlock {
        float threshold;
        float knee;
        float pad[2];
    };

    BlendMode blendModeOf(int blend){
        switch (blend){
            case 1: return BlendMode::ADDITIVE_PREMULTIPLIED;
            case 2: return BlendMode::SCREEN_PREMULTIPLIED;
            case 3: return BlendMode::MULTIPLY_PREMULTIPLIED;
            default: return BlendMode::PREMULTIPLIED;
        }
    }

    float srgbByteToLinear(uint8_t c){
        return EffectValueParser::srgbToLinear((float)c / 255.f);
    }

    bool sameKey(const float a[6], const float b[6]){
        for (int i = 0; i < 6; i++){
            if (a[i] != b[i])
                return false;
        }
        return true;
    }

    // a member by its uniform name (SPIRV-Cross prefixes '_' to one named like a GLSL built-in: noise)
    const ShaderUniform* memberOf(const CustomUniformBlock& block, const std::string& name){
        for (const ShaderUniform& member : block.members){
            if (member.name == name || (member.name.size() == name.size() + 1 && member.name[0] == '_' && member.name.compare(1, std::string::npos, name) == 0))
                return &member;
        }
        return nullptr;
    }

}

EffectRenderer::EffectRenderer(RmlRenderer& renderer): renderer(renderer){
}

EffectRenderer::~EffectRenderer(){
    releaseGpuResources();
}

void EffectRenderer::releaseGpuResources(){
    for (auto& entry : programs)
        entry.second->object.destroy();
    programs.clear();
    brightShader.reset();
}

Rml::CompiledShaderHandle EffectRenderer::compileShader(const Rml::Dictionary& parameters){
    auto it = parameters.find("instance");
    if (it == parameters.end() || it->second.GetType() != Rml::Variant::VOIDPTR)
        return 0;
    EffectInstance* instance = static_cast<EffectInstance*>(it->second.Get<void*>());
    if (!instance || !EffectRuntime::get().isLive(instance))
        return 0;
    return reinterpret_cast<Rml::CompiledShaderHandle>(instance) | 1;
}

void EffectRenderer::releaseShader(Rml::CompiledShaderHandle handle){
    EffectRuntime::get().release(instanceOf(handle));
}

Rml::CompiledFilterHandle EffectRenderer::compileFilter(const Rml::Dictionary& parameters){
    return compileShader(parameters);
}

void EffectRenderer::releaseFilter(Rml::CompiledFilterHandle handle){
    EffectRuntime::get().release(instanceOf(handle));
}

EffectRenderer::Program* EffectRenderer::loadProgram(const std::shared_ptr<ShaderRender>& shader, bool filterForm){
    auto& slot = programs[shader.get()];
    if (slot && slot->shaderId == shader->backend.get().id)
        return slot.get();
    if (slot)
        slot->object.destroy();
    slot = std::make_unique<Program>();
    Program& program = *slot;
    program.shader = shader;
    program.shaderId = shader->backend.get().id;
    ShaderData& data = shader->shaderData;
    program.vertexSlot = data.getUniformBlockIndexByName("u_vs_effectVertex");
    program.frameSlot = data.getUniformBlockIndexByName("u_fs_effectFrame");
    program.trailSlot = data.getUniformBlockIndexByName("u_fs_effectTrail");
    program.ripplesSlot = data.getUniformBlockIndexByName("u_fs_effectRipples");
    program.params.resolve(data, "u_fs_effectParams");
    program.sourceSlot = data.getTextureIndexByName("u_effectSource");
    program.blurredSlot = data.getTextureIndexByName("u_effectBlurred");

    TextureRender* white = &renderer.placeholderWhite;
    program.object.beginLoad(PrimitiveType::TRIANGLES);
    program.object.setShader(shader.get());
    if (filterForm){
        // vertexless: a triangle from gl_VertexIndex
        if (program.sourceSlot.first >= 0)
            program.object.addTexture(program.sourceSlot, ShaderStageType::FRAGMENT, white);
        if (program.blurredSlot.first >= 0)
            program.object.addTexture(program.blurredSlot, ShaderStageType::FRAGMENT, white);
        if (!program.object.endLoadDynamic(PrimitiveType::TRIANGLES, IndexType::NONE)){
            Log::error("[HTML UI] could not load the program of a UI effect (filter form)");
            programs.erase(shader.get());
            return nullptr;
        }
        return &program;
    }
    // the RmlUi vertex layout of the geometry programs (RmlRenderer's composite buffers are
    // the template; bindGeometry sets each draw's buffers)
    const unsigned int stride = sizeof(Rml::Vertex);
    program.object.addAttribute(data.getAttrIndex(AttributeType::POSITION), &renderer.compositeVertexBuffer, 2, AttributeDataType::FLOAT, stride, 0, false, false);
    program.object.addAttribute(data.getAttrIndex(AttributeType::COLOR), &renderer.compositeVertexBuffer, 4, AttributeDataType::UNSIGNED_BYTE, stride, 8, true, false);
    program.object.addAttribute(data.getAttrIndex(AttributeType::TEXCOORD1), &renderer.compositeVertexBuffer, 2, AttributeDataType::FLOAT, stride, 12, false, false);
    program.object.setIndex(&renderer.compositeIndexBuffer, AttributeDataType::UNSIGNED_INT, 0);
    if (!program.object.endLoadDynamic(PrimitiveType::TRIANGLES, IndexType::UINT32)){
        Log::error("[HTML UI] could not load the program of a UI effect (paint form)");
        programs.erase(shader.get());
        return nullptr;
    }
    return &program;
}

EffectRenderer::Program* EffectRenderer::programFor(EffectInstance& instance, bool filterForm){
    const ShaderKey key = EffectRegistry::get().shaderKey(*instance.manifest, filterForm);
    const uint16_t customId = ShaderPool::getCustomIdFromKey(key);
    if (!instance.shader || instance.customShaderId != customId){
        instance.shader = ShaderPool::get(ShaderType::UI_EFFECT, ShaderPool::getPropertiesFromKey(key), customId);
        instance.customShaderId = customId;
    }
    if (!instance.shader)
        return nullptr;
    if (!instance.shader->isCreated()){
        // compiling (the pool hands it to its builder again), or missing (reported once by the pool)
        instance.shader = ShaderPool::get(ShaderType::UI_EFFECT, ShaderPool::getPropertiesFromKey(key), customId);
        EffectRuntime::get().mutableStats().compileCalls++;
        EffectRuntime::get().mutableFrameStats().compileCalls++;
        if (!instance.shader || !instance.shader->isCreated())
            return nullptr;
    }
    return loadProgram(instance.shader, filterForm);
}

float EffectRenderer::scaledPx(EffectInstance& instance, const EffectScaledParam& scaled){
    float value = 0.f;
    if (!scaled.param.empty() && instance.snapshot){
        const int index = instance.manifest->paramIndex(scaled.param);
        if (index >= 0 && index < (int)instance.snapshot->values.size()){
            const EffectValue& v = instance.snapshot->values[index];
            if (v.type == EffectParamType::Length && instance.element){
                EffectLengthContext context;
                context.fontSize = instance.element->GetComputedValues().font_size();
                if (Rml::ElementDocument* document = instance.element->GetOwnerDocument())
                    context.rootFontSize = document->GetComputedValues().font_size();
                if (Rml::Context* rml = instance.element->GetContext())
                    context.viewport = Vector2((float)rml->GetDimensions().x, (float)rml->GetDimensions().y);
                context.areaShorterSide = std::min(instance.areaSize.x, instance.areaSize.y);
                value = EffectValueParser::resolveLength(v.length, context);
            }else{
                value = v.f;
            }
        }
    }
    return value * scaled.scale + scaled.addPx;
}

void EffectRenderer::packParams(EffectInstance& instance, Program& program){
    CustomUniformBlock& block = program.params;
    if (block.slot < 0 || !instance.snapshot || !instance.element)
        return;
    Rml::Element* element = instance.element;
    EffectLengthContext lengths;
    lengths.fontSize = element->GetComputedValues().font_size();
    if (Rml::ElementDocument* document = element->GetOwnerDocument())
        lengths.rootFontSize = document->GetComputedValues().font_size();
    if (Rml::Context* context = element->GetContext())
        lengths.viewport = Vector2((float)context->GetDimensions().x, (float)context->GetDimensions().y);
    lengths.areaShorterSide = std::min(instance.areaSize.x, instance.areaSize.y);
    const float key[6] = {instance.areaSize.x, instance.areaSize.y, lengths.fontSize, lengths.rootFontSize, lengths.viewport.x, lengths.viewport.y};
    if (instance.paramsLayout == &block && sameKey(key, instance.paramsKey) && instance.params.size() == block.data.size())
        return;
    std::memcpy(instance.paramsKey, key, sizeof(key));
    instance.paramsLayout = &block;
    instance.params.assign(block.data.size(), 0);

    const EffectManifest& m = *instance.manifest;
    const Rml::Colourb elementColor = element->GetComputedValues().color();
    for (size_t i = 0; i < m.params.size() && i < instance.snapshot->values.size(); i++){
        const EffectParamDef& def = m.params[i];
        const EffectValue& value = instance.snapshot->values[i];
        if (def.type == EffectParamType::Palette){
            const ShaderUniform* colors = memberOf(block, def.uniform);
            const ShaderUniform* stops = memberOf(block, def.uniform + "Stops");
            const ShaderUniform* count = memberOf(block, def.uniform + "Count");
            Vector4 colorValues[8];
            Vector4 stopValues[2] = {Vector4(0.f, 0.f, 0.f, 0.f), Vector4(0.f, 0.f, 0.f, 0.f)};
            const size_t n = std::min<size_t>(value.palette.size(), 8);
            for (size_t k = 0; k < n; k++){
                colorValues[k] = value.palette[k].first;
                float* stop = (k < 4) ? &stopValues[0].x : &stopValues[1].x;
                stop[k % 4] = value.palette[k].second;
            }
            if (colors)
                CustomUniformBlock::writeUniformArray(instance.params, *colors, colorValues, n);
            if (stops)
                CustomUniformBlock::writeUniformArray(instance.params, *stops, stopValues, 2);
            if (count)
                CustomUniformBlock::writeUniform(instance.params, *count, Vector4((float)n, 0.f, 0.f, 0.f));
            continue;
        }
        const ShaderUniform* member = memberOf(block, def.uniform);
        if (!member)
            continue;
        Vector4 v(0.f, 0.f, 0.f, 0.f);
        switch (def.type){
            case EffectParamType::Length: {
                float px = EffectValueParser::resolveLength(value.length, lengths);
                bool clamped = false;
                px = EffectValueParser::clampToRange(def, px, clamped);
                v = Vector4(px, 0.f, 0.f, 0.f);
                break;
            }
            case EffectParamType::Color:
                if (value.currentColor)
                    v = Vector4(srgbByteToLinear(elementColor.red), srgbByteToLinear(elementColor.green), srgbByteToLinear(elementColor.blue),
                                (float)elementColor.alpha / 255.f);
                else
                    v = value.color;
                break;
            case EffectParamType::Position: {
                const float w = instance.areaSize.x > 0.f ? instance.areaSize.x : 1.f;
                const float h = instance.areaSize.y > 0.f ? instance.areaSize.y : 1.f;
                v = Vector4(EffectValueParser::resolveAxis(value.x, w) / w, EffectValueParser::resolveAxis(value.y, h) / h, 0.f, 0.f);
                break;
            }
            case EffectParamType::Integer:
                v = Vector4(std::round(value.f), 0.f, 0.f, 0.f);
                break;
            default:
                v = Vector4(value.f, 0.f, 0.f, 0.f);
                break;
        }
        CustomUniformBlock::writeUniform(instance.params, *member, v);
    }
}

void EffectRenderer::uploadCommon(EffectInstance& instance, Program& program, ObjectRender& object){
    instance.frame.time[2] = renderer.getPixelScale().x;
    if (program.frameSlot >= 0)
        object.applyUniformBlock(program.frameSlot, sizeof(EffectFrameBlock), &instance.frame);
    if (program.params.slot >= 0 && !instance.params.empty())
        object.applyUniformBlock(program.params.slot, (unsigned int)instance.params.size(), instance.params.data());
    if (program.trailSlot >= 0)
        object.applyUniformBlock(program.trailSlot, sizeof(instance.trail), instance.trail);
    if (program.ripplesSlot >= 0)
        object.applyUniformBlock(program.ripplesSlot, sizeof(instance.ripples), instance.ripples);
}

void EffectRenderer::renderShader(Rml::CompiledShaderHandle handle, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation){
    EffectInstance* instance = instanceOf(handle);
    EffectRuntime& runtime = EffectRuntime::get();
    if (!runtime.isLive(instance) || !instance->element || !instance->manifest)
        return;
    runtime.prepare(*instance);
    Program* program = programFor(*instance, false);
    if (!program)
        return;

    if (!renderer.bindTopLayer())
        return;
    const Rml::Rectanglei scissor = renderer.currentScissorTarget();
    if (!scissor.Valid() || scissor.Width() <= 0 || scissor.Height() <= 0)
        return;
    uint32_t indexCount = 0;
    if (!renderer.bindGeometry(geometry, program->object, indexCount))
        return;
    renderer.applyPassViewport(Rml::Rectanglei::FromSize(Rml::Vector2i(std::max(renderer.targetSize.x, 1), std::max(renderer.targetSize.y, 1))));
    renderer.applyPassScissor(scissor);
    packParams(*instance, *program);

    const uint64_t misses = SystemRender::getPipelineCacheMisses();
    if (!program->object.beginDraw(renderer.getGeometryState(blendModeOf(instance->blend))))
        return;
    const uint64_t created = SystemRender::getPipelineCacheMisses() - misses;
    runtime.mutableStats().pipelinesCreated += created;
    runtime.mutableFrameStats().pipelinesCreated += created;

    VertexBlock vertex;
    std::memcpy(vertex.transform, renderer.getCurrentTransform().data(), sizeof(vertex.transform));
    vertex.translate[0] = translation.x;
    vertex.translate[1] = translation.y;
    vertex.translate[2] = 0.f;
    vertex.translate[3] = 0.f;
    if (program->vertexSlot >= 0)
        program->object.applyUniformBlock(program->vertexSlot, sizeof(VertexBlock), &vertex);
    // a paint draw has no layer
    std::memset(instance->frame.layer, 0, sizeof(float) * 4 * 6);
    uploadCommon(*instance, *program, program->object);
    program->object.draw(0, indexCount, 1);
    renderer.stats.drawCalls++;
    renderer.noteColorWrite();
    runtime.mutableStats().paintDraws++;
    runtime.mutableFrameStats().paintDraws++;
}

bool EffectRenderer::brightPass(EffectInstance& instance, RmlFilterContext& context, float threshold){
    (void)instance;
    const ShaderKey key = EffectRegistry::get().brightPassKey();
    if (!brightShader || !brightShader->isCreated())
        brightShader = ShaderPool::get(ShaderType::UI_EFFECT, ShaderPool::getPropertiesFromKey(key), ShaderPool::getCustomIdFromKey(key));
    if (!brightShader || !brightShader->isCreated())
        return false;
    Program* program = loadProgram(brightShader, true);
    if (!program)
        return false;
    program->object.addTexture(program->sourceSlot, ShaderStageType::FRAGMENT, &context.tertiary().getRender().getColorTexture());
    // the soft knee of the kit's bloomExtractSoftKnee: half the threshold
    BrightBlock block{threshold, threshold * 0.5f + 1e-4f, {0.f, 0.f}};
    context.drawFullscreen(context.secondary(), program->object, context.postState(BlendMode::NONE), [&](ObjectRender& object){
        if (program->params.slot >= 0)
            object.applyUniformBlock(program->params.slot, (unsigned int)std::min<size_t>(sizeof(BrightBlock), program->params.data.size()), &block);
    });
    context.blit(context.secondary(), context.tertiary());
    return true;
}

bool EffectRenderer::applyFilter(Rml::CompiledFilterHandle handle, RmlFilterContext& context){
    EffectInstance* instance = instanceOf(handle);
    EffectRuntime& runtime = EffectRuntime::get();
    if (!runtime.isLive(instance) || !instance->element || !instance->manifest)
        return false;
    runtime.prepare(*instance);
    Program* program = programFor(*instance, true);
    if (!program)
        return false;
    const EffectManifest& m = *instance->manifest;
    Rml::Element* element = instance->element;

    // the layer and the window <-> area-local maps (three projections; affine)
    const Rml::Vector2i layer = context.getLayerSize();
    const Rml::Vector2f scale = renderer.getPixelScale();
    EffectFrameBlock& f = instance->frame;
    f.layer[0] = (float)layer.x;
    f.layer[1] = (float)layer.y;
    f.layer[2] = layer.x > 0 ? 1.f / (float)layer.x : 0.f;
    f.layer[3] = layer.y > 0 ? 1.f / (float)layer.y : 0.f;
    const Rml::Vector2f areaOrigin = element->GetAbsoluteOffset(Rml::BoxArea::Border) + instance->areaOffset;
    const Rml::Vector2f p0 = areaOrigin;
    const float step = 64.f;
    Rml::Vector2f l0 = p0, l1 = p0 + Rml::Vector2f(step, 0.f), l2 = p0 + Rml::Vector2f(0.f, step);
    element->Project(l0);
    element->Project(l1);
    element->Project(l2);
    l0 -= areaOrigin;
    l1 -= areaOrigin;
    l2 -= areaOrigin;
    // local = A * window + b
    const float a00 = (l1.x - l0.x) / step, a10 = (l1.y - l0.y) / step;
    const float a01 = (l2.x - l0.x) / step, a11 = (l2.y - l0.y) / step;
    const float bx = l0.x - (a00 * p0.x + a01 * p0.y);
    const float by = l0.y - (a10 * p0.x + a11 * p0.y);
    const float sx = scale.x > 0.f ? scale.x : 1.f;
    const float sy = scale.y > 0.f ? scale.y : 1.f;
    // window = layerPx / scale
    f.toLocalX[0] = a00 / sx; f.toLocalX[1] = a01 / sy; f.toLocalX[2] = bx; f.toLocalX[3] = 0.f;
    f.toLocalY[0] = a10 / sx; f.toLocalY[1] = a11 / sy; f.toLocalY[2] = by; f.toLocalY[3] = 0.f;
    const float det = a00 * a11 - a01 * a10;
    if (std::fabs(det) > 1e-12f){
        // window = A^-1 (local - b), layerPx = window * scale
        const float i00 = a11 / det, i01 = -a01 / det, i10 = -a10 / det, i11 = a00 / det;
        f.toLayerX[0] = i00 * sx; f.toLayerX[1] = i01 * sx; f.toLayerX[2] = -(i00 * bx + i01 * by) * sx; f.toLayerX[3] = 0.f;
        f.toLayerY[0] = i10 * sy; f.toLayerY[1] = i11 * sy; f.toLayerY[2] = -(i10 * bx + i11 * by) * sy; f.toLayerY[3] = 0.f;
    }else{
        std::memset(f.toLayerX, 0, sizeof(f.toLayerX));
        std::memset(f.toLayerY, 0, sizeof(f.toLayerY));
    }
    const Rml::Rectanglei scissor = context.getScissor();
    if (!scissor.Valid() || scissor.Width() <= 0 || scissor.Height() <= 0 || layer.x <= 0 || layer.y <= 0)
        return false;
    f.srcRect[0] = ((float)scissor.Left() + 0.5f) / (float)layer.x;
    f.srcRect[1] = ((float)scissor.Top() + 0.5f) / (float)layer.y;
    f.srcRect[2] = ((float)scissor.Right() - 0.5f) / (float)layer.x;
    f.srcRect[3] = ((float)scissor.Bottom() - 0.5f) / (float)layer.y;
    packParams(*instance, *program);

    // the blur prepass (and its bright pass) into tertiary
    TextureRender* blurred = &context.primary().getRender().getColorTexture();
    if (m.hasPrepass){
        const float sigma = scaledPx(*instance, m.prepassSigma) * sx;
        context.blit(context.primary(), context.tertiary());
        if (!m.prepassThreshold.empty()){
            const int index = m.paramIndex(m.prepassThreshold);
            const float threshold = (index >= 0 && instance->snapshot && index < (int)instance->snapshot->values.size())
                                    ? instance->snapshot->values[index].f : 0.f;
            brightPass(*instance, context, threshold);
        }
        if (sigma >= 0.5f)
            context.blurInPlace(context.tertiary(), context.secondary(), sigma);
        blurred = &context.tertiary().getRender().getColorTexture();
        runtime.mutableStats().prepassPasses++;
        runtime.mutableFrameStats().prepassPasses++;
    }

    if (program->sourceSlot.first >= 0)
        program->object.addTexture(program->sourceSlot, ShaderStageType::FRAGMENT, &context.primary().getRender().getColorTexture());
    if (program->blurredSlot.first >= 0)
        program->object.addTexture(program->blurredSlot, ShaderStageType::FRAGMENT, blurred);
    const uint64_t misses = SystemRender::getPipelineCacheMisses();
    context.drawFullscreen(context.secondary(), program->object, context.postState(BlendMode::NONE), [&](ObjectRender& object){
        uploadCommon(*instance, *program, object);
    });
    const uint64_t created = SystemRender::getPipelineCacheMisses() - misses;
    runtime.mutableStats().pipelinesCreated += created;
    runtime.mutableFrameStats().pipelinesCreated += created;
    context.swap();
    runtime.mutableStats().filterPasses++;
    runtime.mutableFrameStats().filterPasses++;
    return true;
}
