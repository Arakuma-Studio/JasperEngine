// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitlePassKit.h"

#include "pool/ShaderPool.h"
#include "render/CameraRender.h"
#include "render/FramebufferRender.h"
#include "render/ObjectRender.h"
#include "render/PipelineState.h"
#include "render/ShaderRender.h"
#include "render/TextureRender.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::titleart;

// ---- programs and lookups as data ----

ShaderKey titleart::titleProgramKey(TitleProgram program, uint64_t variant) {
    return ShaderPool::getShaderKey(ShaderType::TITLE_ART, (uint64_t)program | variant);
}

const char* titleart::titleProgramName(TitleProgram program) {
    switch (program) {
        case TitleProgram::Copy: return "copy";
        case TitleProgram::Gauss: return "gauss";
        case TitleProgram::Seed: return "seed";
        case TitleProgram::Jfa: return "jfa";
        case TitleProgram::Distance: return "distance";
        case TitleProgram::Bounds: return "bounds";
        case TitleProgram::OuterGlow: return "outer_glow";
        case TitleProgram::InnerGlow: return "inner_glow";
        case TitleProgram::DropShadow: return "drop_shadow";
        case TitleProgram::InnerShadow: return "inner_shadow";
        case TitleProgram::Outline: return "outline";
        case TitleProgram::ColorOverlay: return "color_overlay";
        case TitleProgram::GradientOverlay: return "gradient_overlay";
        case TitleProgram::PatternOverlay: return "pattern_overlay";
        case TitleProgram::Adjust: return "adjust";
        case TitleProgram::Fill: return "fill";
        case TitleProgram::PatternFill: return "pattern_fill";
    }
    return "copy";
}

uint64_t titleart::titleProgramVariant(TitleProgram program, const TitleArtCaps& caps) {
    const bool fixedSeeds = caps.format(TitleBuffer::Seeds).encoding == TitleEncoding::Fixed12_4;
    const bool fixedDistance = caps.format(TitleBuffer::Distance).encoding == TitleEncoding::Fixed16;
    uint64_t variant = 0;
    switch (program) {
        case TitleProgram::Seed:
        case TitleProgram::Jfa:
            if (fixedSeeds) variant |= TITLE_VARIANT_SEEDS_FIXED;
            break;
        case TitleProgram::Distance:
            if (fixedSeeds) variant |= TITLE_VARIANT_SEEDS_FIXED;
            if (fixedDistance) variant |= TITLE_VARIANT_DISTANCE_FIXED;
            break;
        case TitleProgram::Bounds:
        case TitleProgram::GradientOverlay:
            if (fixedSeeds) variant |= TITLE_VARIANT_SEEDS_FIXED;
            break;
        case TitleProgram::OuterGlow:
            if (fixedSeeds) variant |= TITLE_VARIANT_SEEDS_FIXED;
            if (fixedDistance) variant |= TITLE_VARIANT_DISTANCE_FIXED;
            break;
        case TitleProgram::InnerGlow:
        case TitleProgram::Outline:
            if (fixedDistance) variant |= TITLE_VARIANT_DISTANCE_FIXED;
            break;
        default:
            break;
    }
    return variant;
}

uint64_t TitleLutData::hash() const {
    uint64_t h = 14695981039346656037ull;
    auto add = [&h](uint8_t byte) {
        h ^= byte;
        h *= 1099511628211ull;
    };
    add((uint8_t)format);
    // a data texture's shape and filter are part of what is uploaded (a 256 x 1 lookup adds nothing)
    if (width != TITLE_LUT_SIZE || height != 1 || filter != TextureFilter::LINEAR) {
        for (int v : {width, height, (int)filter}) {
            for (int k = 0; k < 4; k++) add((uint8_t)((uint32_t)v >> (8 * k)));
        }
    }
    for (uint8_t b : bytes) add(b);
    return h;
}

// ---- targets ----

namespace {

    class SokolTitleTarget : public TitleTarget {
    public:
        FramebufferRender framebuffer;
        ~SokolTitleTarget() override { framebuffer.destroyFramebuffer(); }
        TextureRender* colorTexture() const override { return &const_cast<FramebufferRender&>(framebuffer).getColorTexture(); }
    };

    class SokolTitleLut : public TitleLut {
    public:
        TextureRender texture;
        ~SokolTitleLut() override { texture.destroyTexture(); }
    };

    const char* const TEXTURE_NAMES[TITLE_PASS_TEXTURES] = {"u_taTex0", "u_taTex1", "u_taTex2", "u_taTex3"};
    const char* const RAW_NAMES[TITLE_PASS_RAW_TEXTURES] = {"u_taRaw0", "u_taRaw1"};
    // S83's vertex-pulling generator programs: the vertex stage's block and data texture
    const char* const VERTEX_BLOCK = "u_vs_titleart";
    const char* const VERTEX_DATA = "u_taVertexData";

}

std::unique_ptr<TitleTarget> TitleSokolTargetAllocator::create(const TargetKey& key, std::string* error) {
    if (key.depth != TitleDepthFormat::None || key.samples > 1) {
        if (error) *error = "the title pass kit makes colour targets only";
        return nullptr;
    }
    auto target = std::make_unique<SokolTitleTarget>();
    FramebufferDesc desc;
    desc.width = key.width;
    desc.height = key.height;
    desc.color = key.format;
    desc.depth = DepthStencilFormat::NONE;
    desc.sampleCount = 1;
    desc.minFilter = desc.magFilter = key.filter;
    desc.wrapU = desc.wrapV = TextureWrap::CLAMP_TO_EDGE;
    desc.label = "titleart";
    if (!target->framebuffer.createFramebuffer(desc)) {
        if (error) *error = "a " + std::to_string(key.width) + " x " + std::to_string(key.height) + " title target could not be made";
        return nullptr;
    }
    return target;
}

TextureRender* titleart::titleTargetTexture(const TargetRef& target) {
    auto* sokol = dynamic_cast<SokolTitleTarget*>(target.get());
    return sokol ? &sokol->framebuffer.getColorTexture() : nullptr;
}

// ---- the kit ----

struct TitlePassKit::Impl {
    struct Program {
        std::shared_ptr<ShaderRender> shader;
        ObjectRender object;
        int blockSlot = -1;
        int vertexBlockSlot = -1;
        unsigned int vertexBlockBytes = 0;
        std::pair<int, int> vertexData = {-1, -1};
        std::pair<int, int> textures[TITLE_PASS_TEXTURES];
        std::pair<int, int> raw[TITLE_PASS_RAW_TEXTURES];
        bool loaded = false;
    };

    TitleArtCaps caps;
    TitleSokolTargetAllocator allocator;
    TitleTargetPool pool;
    std::map<uint64_t, std::unique_ptr<Program>> programs;
    std::unordered_map<uint64_t, std::unique_ptr<SokolTitleLut>> luts;
    std::vector<std::unique_ptr<TargetRef>> lent;
    TextureRender placeholder;
    bool placeholderMade = false;
    CameraRender pass;
    std::string error;
    uint64_t passCount = 0;

    std::string pending;        // why the last load() found its program not ready yet

    void fail(const std::string& why) {
        if (error.empty()) error = why;
    }

    Impl(const TitleArtCaps& c, size_t poolBytes) : caps(c), pool(allocator, poolBytes) {}

    bool ensurePlaceholder() {
        if (placeholderMade) return true;
        static unsigned char texel[4] = {0, 0, 0, 0};
        void* data[1] = {texel};
        size_t size[1] = {4};
        placeholderMade = placeholder.createTexture("titleart placeholder", 1, 1, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, data, size,
                                                    TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::CLAMP_TO_EDGE,
                                                    TextureWrap::CLAMP_TO_EDGE);
        return placeholderMade;
    }

    Program* load(TitleProgram id, uint64_t variant) {
        const uint64_t properties = (uint64_t)id | variant;
        std::unique_ptr<Program>& slot = programs[properties];
        if (!slot) slot = std::make_unique<Program>();
        Program& program = *slot;
        if (program.loaded) return &program;
        const std::string name = ShaderPool::getShaderStr(ShaderType::TITLE_ART, properties);
        if (ShaderPool::isShaderBuildFailed(ShaderType::TITLE_ART, properties)) {
            fail("the title shader " + name + " could not be built");
            return nullptr;
        }
        program.shader = ShaderPool::get(ShaderType::TITLE_ART, properties);
        if (!program.shader || !program.shader->isCreated()) {
            if (ShaderPool::isShaderBuildFailed(ShaderType::TITLE_ART, properties)) fail("the title shader " + name + " could not be built");
            else pending = "the title shader " + name + " is being compiled";
            return nullptr;
        }
        if (!ensurePlaceholder()) {
            fail("the title passes' placeholder texture could not be made");
            return nullptr;
        }
        ShaderData& data = program.shader->shaderData;
        program.blockSlot = data.getUniformBlockIndexByName("u_fs_titleart");
        for (int i = 0; i < TITLE_PASS_TEXTURES; i++) program.textures[i] = data.getTextureIndexByName(TEXTURE_NAMES[i]);
        for (int i = 0; i < TITLE_PASS_RAW_TEXTURES; i++) program.raw[i] = data.getTextureIndexByName(RAW_NAMES[i]);
        program.vertexBlockSlot = data.getUniformBlockIndexByName(VERTEX_BLOCK);
        if (program.vertexBlockSlot >= 0 && data.getUniformBlockMembers(VERTEX_BLOCK, program.vertexBlockBytes) == nullptr) program.vertexBlockBytes = 0;
        program.vertexBlockBytes = std::min<unsigned int>(program.vertexBlockBytes, sizeof(float) * 4 * TITLE_PASS_VEC4S);
        program.vertexData = data.getTextureIndexByName(VERTEX_DATA);
        program.object.beginLoad(PrimitiveType::TRIANGLES);
        program.object.setShader(program.shader.get());
        for (const auto& t : program.textures) {
            if (t.first >= 0) program.object.addTexture(t, ShaderStageType::FRAGMENT, &placeholder);
        }
        for (const auto& t : program.raw) {
            if (t.first >= 0) program.object.addTexture(t, ShaderStageType::FRAGMENT, &placeholder);
        }
        if (program.vertexData.first >= 0) program.object.addTexture(program.vertexData, ShaderStageType::VERTEX, &placeholder);
        // a pass of a render: its pipelines are made at once, never skipped (spec S32 §6.4)
        program.object.setImmediatePipelines(true);
        if (program.blockSlot < 0 || !program.object.endLoadDynamic(PrimitiveType::TRIANGLES, IndexType::NONE)) {
            program.object.destroy();
            fail("the title program " + name + " could not be loaded");
            return nullptr;
        }
        program.loaded = true;
        return &program;
    }

    TextureRender* textureOf(const TitleSource& source) {
        if (source.target) return titleTargetTexture(*source.target);
        if (source.lut) return &static_cast<SokolTitleLut*>(const_cast<TitleLut*>(source.lut))->texture;
        return const_cast<TextureRender*>(source.texture);
    }

    void reset() {
        // the shaders stay in ShaderPool (shared with every other kit and renderer)
        for (auto& entry : programs) {
            if (entry.second->loaded) entry.second->object.destroy();
        }
        programs.clear();
        luts.clear();
        lent.clear();
        pool.clear();
        if (placeholderMade) placeholder.destroyTexture();
        placeholderMade = false;
    }
};

TitlePassKit::TitlePassKit(const TitleArtCaps& caps, size_t poolBytes) : impl(std::make_unique<Impl>(caps, poolBytes)) {
}

TitlePassKit::~TitlePassKit() {
    impl->reset();
}

const TitleArtCaps& TitlePassKit::caps() const {
    return impl->caps;
}

TargetRef* TitlePassKit::acquire(const TargetKey& key) {
    std::string why;
    TargetRef ref = impl->pool.acquire(key, &why);
    if (!ref) {
        impl->fail(why);
        return nullptr;
    }
    impl->lent.push_back(std::make_unique<TargetRef>(std::move(ref)));
    return impl->lent.back().get();
}

void TitlePassKit::release(TargetRef* target) {
    auto it = std::find_if(impl->lent.begin(), impl->lent.end(), [&](const std::unique_ptr<TargetRef>& r) { return r.get() == target; });
    if (it != impl->lent.end()) impl->lent.erase(it);
}

const TitleLut* TitlePassKit::lut(const TitleLutData& data) {
    const uint64_t key = data.hash();
    auto found = impl->luts.find(key);
    if (found != impl->luts.end()) return found->second.get();
    const size_t texel = colorFormatBytes(data.format);
    if (texel == 0 || data.width <= 0 || data.height <= 0 || data.bytes.size() != texel * (size_t)data.width * (size_t)data.height) {
        impl->fail("a title lookup's bytes do not match its size");
        return nullptr;
    }
    auto lut = std::make_unique<SokolTitleLut>();
    void* pixels[1] = {const_cast<uint8_t*>(data.bytes.data())};
    size_t size[1] = {data.bytes.size()};
    if (!lut->texture.createTexture("titleart lut", data.width, data.height, data.format, TextureType::TEXTURE_2D, 1, pixels, size,
                                    data.filter, data.filter, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE)) {
        impl->fail("a title lookup could not be made");
        return nullptr;
    }
    // bounded: a document's ramps and curves, not one per frame
    if (impl->luts.size() >= 256) impl->luts.clear();
    return (impl->luts[key] = std::move(lut)).get();
}

bool TitlePassKit::prepare(TitleProgram program, uint64_t variant) {
    return impl->load(program, variant) != nullptr;
}

TargetKey TitlePassKit::accumulatorKey(int width, int height) const {
    const TitleBufferFormat& f = impl->caps.format(TitleBuffer::Accumulator);
    TargetKey key;
    key.width = width;
    key.height = height;
    key.format = f.format;
    key.filter = f.filter;
    return key;
}

bool TitlePassKit::run(const TitlePass& pass) {
    if (!pass.output || !*pass.output) {
        impl->fail(std::string("title pass ") + pass.label + ": no output target");
        return false;
    }
    impl->pending.clear();
    Impl::Program* program = impl->load(pass.program, pass.variant);
    if (!program) {
        if (!impl->pending.empty()) impl->fail(impl->pending);
        return false;
    }
    const TargetKey& key = pass.output->key();
    FramebufferRender& framebuffer = static_cast<SokolTitleTarget*>(pass.output->get())->framebuffer;

    float block[TITLE_PASS_VEC4S][4];
    std::memcpy(block, pass.params, sizeof(block));
    const float w = (float)key.width, h = (float)key.height;
    block[0][0] = w;
    block[0][1] = h;
    block[0][2] = pass.pxScale;
    block[0][3] = 0.0f;
    std::memcpy(block[1], pass.frameUv, sizeof(block[1]));

    impl->pass.setColorActions(pass.blend == BlendMode::NONE && pass.scissor.empty() ? LoadAction::DONTCARE : LoadAction::LOAD, StoreAction::STORE);
    impl->pass.setDepthStencilActions(LoadAction::DONTCARE, StoreAction::DONTCARE);
    impl->pass.setLabel(pass.label);
    impl->pass.startRenderPass(&framebuffer);
    if (!pass.scissor.empty()) impl->pass.applyScissorTopLeft(pass.scissor.x, pass.scissor.y, pass.scissor.width, pass.scissor.height);
    PipelineState state;
    state.target.color = key.format;
    state.target.depth = DepthStencilFormat::NONE;
    state.target.sampleCount = 1;
    state.blend = pass.blend;
    bool drawn = false;
    if (program->object.beginDraw(state)) {
        for (int i = 0; i < TITLE_PASS_TEXTURES; i++) {
            if (program->textures[i].first < 0) continue;
            TextureRender* t = impl->textureOf(pass.textures[i]);
            program->object.addTexture(program->textures[i], ShaderStageType::FRAGMENT, t ? t : &impl->placeholder);
        }
        for (int i = 0; i < TITLE_PASS_RAW_TEXTURES; i++) {
            if (program->raw[i].first < 0) continue;
            TextureRender* t = impl->textureOf(pass.raw[i]);
            program->object.addTexture(program->raw[i], ShaderStageType::FRAGMENT, t ? t : &impl->placeholder);
        }
        if (program->vertexData.first >= 0) {
            TextureRender* t = impl->textureOf(pass.vertexData);
            program->object.addTexture(program->vertexData, ShaderStageType::VERTEX, t ? t : &impl->placeholder);
        }
        program->object.applyUniformBlock(program->blockSlot, sizeof(block), block);
        // the vertex stage's block mirrors the first vec4s of the fragment's (TitleGen.h)
        if (program->vertexBlockSlot >= 0 && program->vertexBlockBytes > 0) program->object.applyUniformBlock(program->vertexBlockSlot, program->vertexBlockBytes, block);
        program->object.draw(0, (unsigned int)std::max(3, pass.vertices), 1);
        drawn = true;
    }
    impl->pass.endRenderPass();
    if (!drawn) {
        impl->fail(std::string("title pass ") + pass.label + ": its pipeline could not be made");
        return false;
    }
    impl->passCount++;
    return true;
}

bool TitlePassKit::clear(TargetRef& target) {
    if (!target) {
        impl->fail("clear: no target");
        return false;
    }
    FramebufferRender& framebuffer = static_cast<SokolTitleTarget*>(target.get())->framebuffer;
    impl->pass.setColorActions(LoadAction::CLEAR, StoreAction::STORE, Vector4(0.0f, 0.0f, 0.0f, 0.0f));
    impl->pass.setDepthStencilActions(LoadAction::DONTCARE, StoreAction::DONTCARE);
    impl->pass.setLabel("titleart clear");
    impl->pass.startRenderPass(&framebuffer);
    impl->pass.endRenderPass();
    return true;
}

const std::string& TitlePassKit::error() const {
    return impl->error;
}

void TitlePassKit::clearError() {
    impl->error.clear();
}

uint64_t TitlePassKit::passes() const {
    return impl->passCount;
}

TitleTargetPool& TitlePassKit::pool() {
    return impl->pool;
}

void TitlePassKit::reset() {
    impl->reset();
}
